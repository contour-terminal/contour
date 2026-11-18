// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/VectorGlyph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <ranges>
#include <span>
#include <utility>

namespace vtbackend
{

namespace
{
    using std::string_view;

    /// The first thing that went wrong while reading a payload. Readers keep going with a value of no
    /// meaning once it is set, and their caller asks before using anything: that spares every read of a
    /// number its own early return, and reports what the first failure was, as stopping there would.
    class Failure
    {
      public:
        void fail(VectorGlyphError error) noexcept
        {
            if (!_error)
                _error = error;
        }

        [[nodiscard]] bool failed() const noexcept { return _error.has_value(); }
        [[nodiscard]] VectorGlyphError error() const noexcept
        {
            return _error.value_or(VectorGlyphError::Malformed);
        }

      private:
        std::optional<VectorGlyphError> _error;
    };

    [[nodiscard]] constexpr bool isDigit(char ch) noexcept
    {
        return ch >= '0' && ch <= '9';
    }

    /// The payload is printable ASCII, so the space is the only whitespace there is.
    [[nodiscard]] constexpr string_view trim(string_view text) noexcept
    {
        auto const first = text.find_first_not_of(' ');
        if (first == string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(' ') - first + 1);
    }

    /// The exact powers of ten a double holds.
    constexpr auto PowersOfTen =
        std::array { 1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                     1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

    /// Converts the text of an SVG number, already known to match the grammar, without regard to locale:
    /// `strtod` reads a comma as the decimal point where the user's locale has one.
    [[nodiscard]] double numberValue(string_view text) noexcept
    {
        auto const negative = text.starts_with('-');
        if (negative || text.starts_with('+'))
            text.remove_prefix(1);

        // Up to nineteen significant digits fit; the rest only move the decimal point.
        constexpr auto MantissaLimit = uint64_t { 1'000'000'000'000'000'000 };
        auto mantissa = uint64_t { 0 };
        auto exponent = 0;
        auto index = size_t { 0 };
        auto const takeDigits = [&](int perKeptDigit, int perDroppedDigit) {
            while (index < text.size() && isDigit(text[index]))
            {
                if (mantissa < MantissaLimit)
                {
                    mantissa = (mantissa * 10) + static_cast<uint64_t>(text[index] - '0');
                    exponent += perKeptDigit;
                }
                else
                    exponent += perDroppedDigit;
                ++index;
            }
        };
        takeDigits(0, 1);
        if (index < text.size() && text[index] == '.')
        {
            ++index;
            takeDigits(-1, 0);
        }
        if (index < text.size() && (text[index] == 'e' || text[index] == 'E'))
        {
            ++index;
            auto const negativeExponent = index < text.size() && text[index] == '-';
            if (index < text.size() && (text[index] == '-' || text[index] == '+'))
                ++index;
            // Past a few hundred the value is zero or infinite whatever follows.
            constexpr auto ExponentLimit = 10'000;
            auto stated = 0;
            while (index < text.size() && isDigit(text[index]))
            {
                stated = std::min(ExponentLimit, (stated * 10) + (text[index] - '0'));
                ++index;
            }
            exponent += negativeExponent ? -stated : stated;
        }

        // Scaled by exact powers of ten, so the common short numbers come out correctly rounded.
        constexpr auto LargestStep = static_cast<int>(PowersOfTen.size()) - 1;
        auto value = static_cast<double>(mantissa);
        exponent = std::clamp(exponent, -400, 400);
        while (exponent > 0 && std::isfinite(value) && value != 0)
        {
            auto const step = std::min(exponent, LargestStep);
            value *= PowersOfTen[static_cast<size_t>(step)];
            exponent -= step;
        }
        while (exponent < 0 && value != 0)
        {
            auto const step = std::min(-exponent, LargestStep);
            value /= PowersOfTen[static_cast<size_t>(step)];
            exponent += step;
        }
        return negative ? -value : value;
    }

    /// Reads numbers, flags and separators off a piece of a payload, as the SVG 2 path grammar spells them.
    class Cursor
    {
      public:
        Cursor(string_view text, Failure& failure) noexcept: _text { text }, _failure { failure } {}

        [[nodiscard]] bool atEnd() const noexcept { return _position >= _text.size(); }
        [[nodiscard]] char peek() const noexcept { return atEnd() ? '\0' : _text[_position]; }
        [[nodiscard]] string_view rest() const noexcept
        {
            return _text.substr(std::min(_position, _text.size()));
        }
        void advance(size_t count = 1) noexcept { _position += count; }

        void skipSpaces() noexcept
        {
            while (peek() == ' ')
                ++_position;
        }

        /// SVG's optional `comma_wsp`: spaces, at most one comma, spaces.
        void skipCommaSpaces() noexcept
        {
            skipSpaces();
            if (peek() == ',')
            {
                ++_position;
                skipSpaces();
            }
        }

        /// After a complete set of arguments: is another one coming? A comma commits to it.
        [[nodiscard]] bool moreArguments() noexcept
        {
            if (_failure.failed())
                return false;
            skipSpaces();
            if (peek() == ',')
            {
                ++_position;
                skipSpaces();
                return true;
            }
            return isDigit(peek()) || peek() == '.' || peek() == '+' || peek() == '-';
        }

        /// SVG 2's `number`: a sign, then digits, digits with a fraction, or a fraction alone, then an
        /// exponent. A `.` with no digit after it is not part of the number, nor is an `e` without one.
        [[nodiscard]] float number() noexcept
        {
            auto const start = _position;
            if (peek() == '+' || peek() == '-')
                ++_position;
            auto const wholeDigits = digits();
            auto fractionDigits = size_t { 0 };
            if (peek() == '.')
            {
                auto const point = _position;
                ++_position;
                fractionDigits = digits();
                if (fractionDigits == 0)
                    _position = point;
            }
            if (wholeDigits == 0 && fractionDigits == 0)
            {
                _position = start;
                _failure.fail(VectorGlyphError::Malformed);
                return 0;
            }
            if (peek() == 'e' || peek() == 'E')
            {
                auto const mark = _position;
                ++_position;
                if (peek() == '+' || peek() == '-')
                    ++_position;
                if (digits() == 0)
                    _position = mark;
            }
            auto const value = static_cast<float>(numberValue(_text.substr(start, _position - start)));
            // Every number fits a 32-bit float.
            if (!std::isfinite(value))
            {
                _failure.fail(VectorGlyphError::TooLarge);
                return 0;
            }
            return value;
        }

        /// A number that is the whole of the text.
        [[nodiscard]] float wholeNumber() noexcept
        {
            auto const value = number();
            if (!atEnd())
                _failure.fail(VectorGlyphError::Malformed);
            return value;
        }

        /// An arc flag: `0` or `1`, which needs no separator after it.
        [[nodiscard]] bool flag() noexcept
        {
            if (peek() != '0' && peek() != '1')
            {
                _failure.fail(VectorGlyphError::Malformed);
                return false;
            }
            return _text[_position++] == '1';
        }

        [[nodiscard]] std::pair<double, double> pair() noexcept
        {
            auto const x = double { number() };
            skipCommaSpaces();
            auto const y = double { number() };
            return { x, y };
        }

      private:
        [[nodiscard]] size_t digits() noexcept
        {
            auto const start = _position;
            while (isDigit(peek()))
                ++_position;
            return _position - start;
        }

        string_view _text;
        size_t _position = 0;
        Failure& _failure;
    };

    /// The least and the most a matrix stretches a length by: its singular values.
    [[nodiscard]] std::pair<double, double> stretchOf(VectorMatrix const& matrix) noexcept
    {
        auto const a = double { matrix.a };
        auto const b = double { matrix.b };
        auto const c = double { matrix.c };
        auto const d = double { matrix.d };
        auto const sum = (a * a) + (b * b) + (c * c) + (d * d);
        auto const determinant = std::abs((a * d) - (b * c));
        auto const most = std::sqrt(
            (sum + std::sqrt(std::max(0.0, (sum * sum) - (4.0 * determinant * determinant)))) / 2.0);
        auto const least = most > 0.0 ? determinant / most : 0.0;
        return { least, most };
    }

    /// Six numbers after an `m`: an affine matrix that stretches no length by more than
    /// MaxVectorMatrixStretch or by less than its inverse, so it is invertible too.
    [[nodiscard]] VectorMatrix readMatrix(Cursor& cursor, Failure& failure) noexcept
    {
        cursor.skipSpaces();
        auto values = std::array<float, 6> {};
        for (auto const index: std::views::iota(size_t { 0 }, values.size()))
        {
            if (index > 0)
                cursor.skipCommaSpaces();
            values[index] = cursor.number();
        }
        auto const matrix = VectorMatrix {
            .a = values[0], .b = values[1], .c = values[2], .d = values[3], .e = values[4], .f = values[5]
        };
        auto const [least, most] = stretchOf(matrix);
        // Negated, so that a NaN fails as well.
        if (!(least >= 1.0 / MaxVectorMatrixStretch && most <= MaxVectorMatrixStretch))
            failure.fail(VectorGlyphError::Malformed);
        return matrix;
    }

    /// The box every point must stay in: the viewBox and one of its sizes around it.
    struct Bounds
    {
        float left;
        float top;
        float right;
        float bottom;

        explicit Bounds(VectorViewBox const& viewBox) noexcept:
            left { viewBox.x - (std::max(viewBox.width, viewBox.height) * MaxVectorExtentMultiple) },
            top { viewBox.y - (std::max(viewBox.width, viewBox.height) * MaxVectorExtentMultiple) },
            right { viewBox.x + viewBox.width
                    + (std::max(viewBox.width, viewBox.height) * MaxVectorExtentMultiple) },
            bottom { viewBox.y + viewBox.height
                     + (std::max(viewBox.width, viewBox.height) * MaxVectorExtentMultiple) }
        {
        }

        /// @return Whether the point lies inside; a NaN does not.
        [[nodiscard]] bool contains(float x, float y) const noexcept
        {
            return x >= left && x <= right && y >= top && y <= bottom;
        }
    };

    /// What a registration may still spend, shared by its layers.
    struct Budget
    {
        size_t segments = MaxVectorSegments;
        double ink = 0; ///< Path length left, in viewBox units.
    };

    struct Position
    {
        double x = 0;
        double y = 0;
    };

    [[nodiscard]] double distance(Position from, Position to) noexcept
    {
        return std::hypot(to.x - from.x, to.y - from.y);
    }

    /// Collects a layer's segments, charging each against the registration's budget.
    class PathWriter
    {
      public:
        PathWriter(Bounds const& bounds,
                   Budget& budget,
                   double strokeWidth,
                   std::optional<VectorMatrix> const& transform,
                   Failure& failure) noexcept:
            _bounds { bounds },
            _budget { budget },
            _strokeWidth { strokeWidth },
            _transform { transform },
            _failure { failure }
        {
        }

        /// @return The subpath's start, in the layer's coordinates.
        [[nodiscard]] Position start() const noexcept { return _start; }
        [[nodiscard]] std::vector<VectorSegment> take() noexcept { return std::move(_segments); }

        void moveTo(Position to)
        {
            spendSegment();
            auto const placed = place(to);
            // No length, but a stroked subpath gets caps.
            spendInk(0.0);
            push(VectorSegmentKind::MoveTo, { placed.stored });
            _start = Position { .x = double { placed.stored.x }, .y = double { placed.stored.y } };
            _startInViewBox = placed.inViewBox;
            _pen = placed.inViewBox;
            _needsMove = false;
        }

        void lineTo(Position to)
        {
            ensureOpen();
            spendSegment();
            auto const placed = place(to);
            spendInk(distance(_pen, placed.inViewBox));
            _pen = placed.inViewBox;
            push(VectorSegmentKind::LineTo, { placed.stored });
        }

        void quadTo(Position control, Position to)
        {
            ensureOpen();
            spendSegment();
            auto const placedControl = place(control);
            auto const placedTo = place(to);
            // A curve's control polygon bounds its length.
            spendInk(distance(_pen, placedControl.inViewBox)
                     + distance(placedControl.inViewBox, placedTo.inViewBox));
            _pen = placedTo.inViewBox;
            push(VectorSegmentKind::QuadTo, { placedControl.stored, placedTo.stored });
        }

        void cubicTo(Position control1, Position control2, Position to)
        {
            ensureOpen();
            spendSegment();
            auto const placed1 = place(control1);
            auto const placed2 = place(control2);
            auto const placedTo = place(to);
            spendInk(distance(_pen, placed1.inViewBox) + distance(placed1.inViewBox, placed2.inViewBox)
                     + distance(placed2.inViewBox, placedTo.inViewBox));
            _pen = placedTo.inViewBox;
            push(VectorSegmentKind::CubicTo, { placed1.stored, placed2.stored, placedTo.stored });
        }

        /// A `Z` right after another closes nothing, and costs nothing.
        void close()
        {
            if (_needsMove)
                return;
            spendSegment();
            spendInk(distance(_pen, _startInViewBox));
            _pen = _startInViewBox;
            push(VectorSegmentKind::Close, {});
            _needsMove = true;
        }

      private:
        /// A point as it is stored (the layer's coordinates) and as it is judged (the viewBox's).
        struct Placed
        {
            VectorPoint stored;
            Position inViewBox;
        };

        /// Every segment is charged, moves and closes included: otherwise `M0 0Z` repeated would fill a
        /// payload with segments the cap never sees.
        void spendSegment() noexcept
        {
            if (_budget.segments == 0)
                _failure.fail(VectorGlyphError::TooLarge);
            else
                --_budget.segments;
        }

        /// Charges a length of path, plus a stroke's width for the join or cap drawn around its end.
        void spendInk(double length) noexcept
        {
            _budget.ink -= length + _strokeWidth;
            if (_budget.ink < 0.0)
                _failure.fail(VectorGlyphError::TooLarge);
        }

        /// Bounds and path length apply after the layer's matrix.
        [[nodiscard]] Placed place(Position point) noexcept
        {
            auto const inViewBox =
                _transform
                    ? Position { .x = (double { _transform->a } * point.x)
                                      + (double { _transform->c } * point.y) + double { _transform->e },
                                 .y = (double { _transform->b } * point.x)
                                      + (double { _transform->d } * point.y) + double { _transform->f } }
                    : point;
            auto const x = static_cast<float>(inViewBox.x);
            auto const y = static_cast<float>(inViewBox.y);
            if (!_bounds.contains(x, y))
                _failure.fail(VectorGlyphError::TooLarge);
            return Placed { .stored = VectorPoint { .x = static_cast<float>(point.x),
                                                    .y = static_cast<float>(point.y) },
                            .inViewBox = Position { .x = double { x }, .y = double { y } } };
        }

        /// A drawing command right after `Z` starts a new subpath at the closed one's start, as SVG says.
        void ensureOpen()
        {
            if (_needsMove)
                moveTo(_start);
        }

        void push(VectorSegmentKind kind, std::initializer_list<VectorPoint> points)
        {
            // Nothing is kept of a payload that is already refused.
            if (_failure.failed())
                return;
            auto segment = VectorSegment { .kind = kind, .points = {} };
            std::ranges::copy(points, segment.points.begin());
            _segments.push_back(segment);
        }

        std::vector<VectorSegment> _segments;
        Bounds const& _bounds;
        Budget& _budget;
        double _strokeWidth;
        std::optional<VectorMatrix> const& _transform;
        Failure& _failure;
        Position _pen {};            ///< Where the last segment ended, in viewBox units.
        bool _needsMove = false;     ///< A `Z` closed the subpath and no `M` has opened the next one.
        Position _start {};          ///< The subpath's start, in the layer's coordinates.
        Position _startInViewBox {}; ///< And in viewBox units.
    };

    /// The parameters of one elliptical arc command.
    struct Arc
    {
        Position from;
        double radiusX = 0;
        double radiusY = 0;
        double rotationDegrees = 0;
        bool largeArc = false;
        bool sweep = false;
        Position to;
    };

    /// Appends an SVG elliptical arc as cubics, by SVG 2's implementation notes: identical endpoints draw
    /// nothing, a zero radius draws a line, and radii too small for the endpoints are scaled up. One cubic
    /// for each quarter turn or part of one, so that the segment limit counts the same everywhere.
    void arcTo(PathWriter& out, Arc const& arc, Failure& failure)
    {
        if (arc.from.x == arc.to.x && arc.from.y == arc.to.y)
            return;
        auto radiusX = std::abs(arc.radiusX);
        auto radiusY = std::abs(arc.radiusY);
        if (radiusX == 0.0 || radiusY == 0.0)
        {
            out.lineTo(arc.to);
            return;
        }
        auto const rotation = arc.rotationDegrees * std::numbers::pi / 180.0;
        auto const sinPhi = std::sin(rotation);
        auto const cosPhi = std::cos(rotation);

        // The midpoint, in the ellipse's rotated frame.
        auto const halfX = (arc.from.x - arc.to.x) / 2.0;
        auto const halfY = (arc.from.y - arc.to.y) / 2.0;
        auto const x1 = (cosPhi * halfX) + (sinPhi * halfY);
        auto const y1 = (-sinPhi * halfX) + (cosPhi * halfY);

        // Radii out of range: scaled up until the endpoints fit.
        auto const lambda = ((x1 * x1) / (radiusX * radiusX)) + ((y1 * y1) / (radiusY * radiusY));
        if (lambda > 1.0)
        {
            radiusX *= std::sqrt(lambda);
            radiusY *= std::sqrt(lambda);
        }

        // The center in the rotated frame. Rounding can push the radicand a little below zero when the
        // radii were just scaled.
        auto const rx2 = radiusX * radiusX;
        auto const ry2 = radiusY * radiusY;
        auto const numerator = (rx2 * ry2) - (rx2 * y1 * y1) - (ry2 * x1 * x1);
        auto const denominator = (rx2 * y1 * y1) + (ry2 * x1 * x1);
        auto coefficient = denominator > 0.0 ? std::sqrt(std::max(0.0, numerator / denominator)) : 0.0;
        if (arc.largeArc == arc.sweep)
            coefficient = -coefficient;
        auto const centerX1 = coefficient * radiusX * y1 / radiusY;
        auto const centerY1 = -coefficient * radiusY * x1 / radiusX;

        // The center, and the start and sweep angles.
        auto const centerX = (cosPhi * centerX1) - (sinPhi * centerY1) + ((arc.from.x + arc.to.x) / 2.0);
        auto const centerY = (sinPhi * centerX1) + (cosPhi * centerY1) + ((arc.from.y + arc.to.y) / 2.0);
        auto const angle = [](double ux, double uy, double vx, double vy) {
            return std::atan2((ux * vy) - (uy * vx), (ux * vx) + (uy * vy));
        };
        auto const ux = (x1 - centerX1) / radiusX;
        auto const uy = (y1 - centerY1) / radiusY;
        auto const vx = (-x1 - centerX1) / radiusX;
        auto const vy = (-y1 - centerY1) / radiusY;
        auto const startAngle = angle(1.0, 0.0, ux, uy);
        auto const fullTurn = 2.0 * std::numbers::pi;
        auto delta = std::fmod(angle(ux, uy, vx, vy), fullTurn);
        if (!arc.sweep && delta > 0.0)
            delta -= fullTurn;
        else if (arc.sweep && delta < 0.0)
            delta += fullTurn;
        if (!std::isfinite(delta))
        {
            failure.fail(VectorGlyphError::Malformed);
            return;
        }

        auto const quarterTurn = std::numbers::pi / 2.0;
        auto const pieces = std::clamp(static_cast<int>(std::ceil(std::abs(delta) / quarterTurn)), 1, 4);
        auto const step = delta / pieces;
        auto const kappa = 4.0 / 3.0 * std::tan(step / 4.0);
        auto const pointAt = [&](double t) {
            return Position {
                .x = centerX + (radiusX * std::cos(t) * cosPhi) - (radiusY * std::sin(t) * sinPhi),
                .y = centerY + (radiusX * std::cos(t) * sinPhi) + (radiusY * std::sin(t) * cosPhi)
            };
        };
        auto const tangentAt = [&](double t) {
            return Position { .x = (-radiusX * std::sin(t) * cosPhi) - (radiusY * std::cos(t) * sinPhi),
                              .y = (-radiusX * std::sin(t) * sinPhi) + (radiusY * std::cos(t) * cosPhi) };
        };
        auto t = startAngle;
        auto from = arc.from;
        for (auto const piece: std::views::iota(0, pieces))
        {
            auto const next = t + step;
            // The last piece lands exactly on the endpoint the path data gave.
            auto const to = piece + 1 == pieces ? arc.to : pointAt(next);
            auto const leaving = tangentAt(t);
            auto const arriving = tangentAt(next);
            out.cubicTo(Position { .x = from.x + (kappa * leaving.x), .y = from.y + (kappa * leaving.y) },
                        Position { .x = to.x - (kappa * arriving.x), .y = to.y - (kappa * arriving.y) },
                        to);
            from = to;
            t = next;
        }
    }

    /// The control point a following `S` or `T` reflects.
    struct LastControl
    {
        enum class Kind : uint8_t
        {
            None,
            Cubic,
            Quad,
        };

        Kind kind = Kind::None;
        Position point {};
    };

    /// Reads SVG 2 path data into a layer's segments.
    [[nodiscard]] std::vector<VectorSegment> parsePathData(string_view text,
                                                           Bounds const& bounds,
                                                           Budget& budget,
                                                           double strokeWidth,
                                                           std::optional<VectorMatrix> const& transform,
                                                           Failure& failure)
    {
        auto cursor = Cursor { text, failure };
        auto out = PathWriter { bounds, budget, strokeWidth, transform, failure };
        auto current = Position {};
        auto last = LastControl {};

        cursor.skipSpaces();
        // Path data starts with a moveto.
        if (cursor.peek() != 'M' && cursor.peek() != 'm')
            failure.fail(VectorGlyphError::Malformed);

        while (!failure.failed())
        {
            cursor.skipSpaces();
            if (cursor.atEnd())
                break;
            auto const command = cursor.peek();
            cursor.advance();
            auto const relative = command >= 'a' && command <= 'z';
            // What a relative command's coordinates are measured from: the current point as the command's
            // argument set begins.
            auto const origin = [&] {
                return relative ? current : Position {};
            };
            auto const pairFrom = [&](Position base) {
                auto const [x, y] = cursor.pair();
                return Position { .x = base.x + x, .y = base.y + y };
            };
            // The control point `S` and `T` take: the last one's reflection, when the last command was of
            // their own kind.
            auto const reflected = [&](LastControl::Kind kind) {
                return last.kind == kind ? Position { .x = (2.0 * current.x) - last.point.x,
                                                      .y = (2.0 * current.y) - last.point.y }
                                         : current;
            };
            cursor.skipSpaces();
            switch (relative ? static_cast<char>(command - 'a' + 'A') : command)
            {
                case 'M': {
                    current = pairFrom(origin());
                    out.moveTo(current);
                    // Further pairs are implicit linetos.
                    while (cursor.moreArguments())
                    {
                        current = pairFrom(origin());
                        out.lineTo(current);
                    }
                    last = {};
                    break;
                }
                case 'Z':
                    out.close();
                    current = out.start();
                    last = {};
                    break;
                case 'L':
                    do
                    {
                        current = pairFrom(origin());
                        out.lineTo(current);
                    } while (cursor.moreArguments());
                    last = {};
                    break;
                case 'H':
                    do
                    {
                        current.x = double { cursor.number() } + (relative ? current.x : 0.0);
                        out.lineTo(current);
                    } while (cursor.moreArguments());
                    last = {};
                    break;
                case 'V':
                    do
                    {
                        current.y = double { cursor.number() } + (relative ? current.y : 0.0);
                        out.lineTo(current);
                    } while (cursor.moreArguments());
                    last = {};
                    break;
                case 'C':
                    do
                    {
                        auto const base = origin();
                        auto const control1 = pairFrom(base);
                        cursor.skipCommaSpaces();
                        auto const control2 = pairFrom(base);
                        cursor.skipCommaSpaces();
                        auto const to = pairFrom(base);
                        out.cubicTo(control1, control2, to);
                        current = to;
                        last = { .kind = LastControl::Kind::Cubic, .point = control2 };
                    } while (cursor.moreArguments());
                    break;
                case 'S':
                    do
                    {
                        auto const base = origin();
                        auto const control1 = reflected(LastControl::Kind::Cubic);
                        auto const control2 = pairFrom(base);
                        cursor.skipCommaSpaces();
                        auto const to = pairFrom(base);
                        out.cubicTo(control1, control2, to);
                        current = to;
                        last = { .kind = LastControl::Kind::Cubic, .point = control2 };
                    } while (cursor.moreArguments());
                    break;
                case 'Q':
                    do
                    {
                        auto const base = origin();
                        auto const control = pairFrom(base);
                        cursor.skipCommaSpaces();
                        auto const to = pairFrom(base);
                        out.quadTo(control, to);
                        current = to;
                        last = { .kind = LastControl::Kind::Quad, .point = control };
                    } while (cursor.moreArguments());
                    break;
                case 'T':
                    do
                    {
                        auto const control = reflected(LastControl::Kind::Quad);
                        auto const to = pairFrom(origin());
                        out.quadTo(control, to);
                        current = to;
                        last = { .kind = LastControl::Kind::Quad, .point = control };
                    } while (cursor.moreArguments());
                    break;
                case 'A':
                    do
                    {
                        auto arc = Arc { .from = current };
                        arc.radiusX = double { cursor.number() };
                        cursor.skipCommaSpaces();
                        arc.radiusY = double { cursor.number() };
                        cursor.skipCommaSpaces();
                        arc.rotationDegrees = double { cursor.number() };
                        cursor.skipCommaSpaces();
                        arc.largeArc = cursor.flag();
                        cursor.skipCommaSpaces();
                        arc.sweep = cursor.flag();
                        cursor.skipCommaSpaces();
                        arc.to = pairFrom(origin());
                        // A number that could not be read leaves nothing to draw an arc from.
                        if (!failure.failed())
                            arcTo(out, arc, failure);
                        current = arc.to;
                        last = {};
                    } while (cursor.moreArguments());
                    break;
                default: failure.fail(VectorGlyphError::Malformed); break;
            }
        }
        return out.take();
    }

    /// One name of an enumeration, as it is spelled in a payload.
    template <typename Value>
    struct Named
    {
        string_view name;
        Value value;
    };

    constexpr auto FillRules = std::array {
        Named { .name = "nonzero", .value = VectorFillRule::NonZero },
        Named { .name = "evenodd", .value = VectorFillRule::EvenOdd },
    };

    constexpr auto LineCaps = std::array {
        Named { .name = "round", .value = VectorLineCap::Round },
        Named { .name = "butt", .value = VectorLineCap::Butt },
        Named { .name = "square", .value = VectorLineCap::Square },
    };

    constexpr auto Spreads = std::array {
        Named { .name = "pad", .value = VectorSpread::Pad },
        Named { .name = "reflect", .value = VectorSpread::Reflect },
        Named { .name = "repeat", .value = VectorSpread::Repeat },
    };

    /// @return The value @p name spells; a name the table does not hold fails the payload.
    template <typename Value, size_t N>
    [[nodiscard]] Value named(std::array<Named<Value>, N> const& table,
                              string_view name,
                              Failure& failure) noexcept
    {
        auto const found = std::ranges::find(table, name, &Named<Value>::name);
        if (found == table.end())
        {
            failure.fail(VectorGlyphError::Malformed);
            return table.front().value;
        }
        return found->value;
    }

    [[nodiscard]] constexpr std::optional<uint8_t> hexDigit(char ch) noexcept
    {
        if (isDigit(ch))
            return static_cast<uint8_t>(ch - '0');
        if (ch >= 'a' && ch <= 'f')
            return static_cast<uint8_t>(ch - 'a' + 10);
        if (ch >= 'A' && ch <= 'F')
            return static_cast<uint8_t>(ch - 'A' + 10);
        return std::nullopt;
    }

    /// `rrggbb` or `rrggbbaa`, hex digits in either case.
    [[nodiscard]] VectorFixedColor parseHexColor(string_view hex, Failure& failure) noexcept
    {
        auto channels = std::array<uint8_t, 4> { 0, 0, 0, 0xFF };
        if (hex.size() != 6 && hex.size() != 8)
            failure.fail(VectorGlyphError::Malformed);
        for (auto const index: std::views::iota(size_t { 0 }, std::min(hex.size() / 2, channels.size())))
        {
            auto const high = hexDigit(hex[index * 2]);
            auto const low = hexDigit(hex[(index * 2) + 1]);
            if (!high || !low)
            {
                failure.fail(VectorGlyphError::Malformed);
                break;
            }
            channels[index] = static_cast<uint8_t>((*high << 4) | *low);
        }
        return VectorFixedColor {
            .red = channels[0], .green = channels[1], .blue = channels[2], .alpha = channels[3]
        };
    }

    /// `fg`, `fg@<alpha>`, `#rrggbb` or `#rrggbbaa`.
    [[nodiscard]] VectorSolid parseSolid(string_view text, Failure& failure) noexcept
    {
        if (text == "fg")
            return VectorForeground {};
        if (text.starts_with("fg@"))
        {
            auto const alpha = Cursor { text.substr(3), failure }.wholeNumber();
            if (!(alpha >= 0.0F && alpha <= 1.0F))
                failure.fail(VectorGlyphError::Malformed);
            return VectorForeground { .alpha = static_cast<uint8_t>(
                                          std::lround(std::clamp(alpha, 0.0F, 1.0F) * 255.0F)) };
        }
        if (!text.starts_with('#'))
        {
            failure.fail(VectorGlyphError::Malformed);
            return VectorForeground {};
        }
        return parseHexColor(text.substr(1), failure);
    }

    /// Which of a gradient's two shapes a colour spells.
    enum class GradientKind : uint8_t
    {
        Linear,
        Radial,
    };

    /// The inside of `lin(...)` or `rad(...)`, the closing parenthesis included.
    [[nodiscard]] VectorGradient parseGradient(string_view body, GradientKind kind, Failure& failure)
    {
        auto gradient = VectorGradient {};
        if (!body.ends_with(')'))
        {
            failure.fail(VectorGlyphError::Malformed);
            return gradient;
        }
        body.remove_suffix(1);

        // The geometry, then a spread and a matrix; the stops follow, each after a `/`.
        auto const firstStop = body.find('/');
        auto cursor = Cursor { body.substr(0, firstStop), failure };
        cursor.skipSpaces();
        auto geometry = std::array<float, 6> {};
        auto const count = kind == GradientKind::Radial ? size_t { 6 } : size_t { 4 };
        for (auto const index: std::views::iota(size_t { 0 }, count))
        {
            if (index > 0)
                cursor.skipCommaSpaces();
            geometry[index] = cursor.number();
        }
        if (kind == GradientKind::Radial)
        {
            if (geometry[2] < 0.0F || geometry[5] < 0.0F)
                failure.fail(VectorGlyphError::Malformed);
            gradient.shape = VectorRadialGradient { .focalX = geometry[0],
                                                    .focalY = geometry[1],
                                                    .focalRadius = geometry[2],
                                                    .centerX = geometry[3],
                                                    .centerY = geometry[4],
                                                    .radius = geometry[5] };
        }
        else
            gradient.shape = VectorLinearGradient {
                .x1 = geometry[0], .y1 = geometry[1], .x2 = geometry[2], .y2 = geometry[3]
            };

        cursor.skipSpaces();
        for (auto const& spread: Spreads)
        {
            // A whole word: `padm 1 0 0 1 0 0` is not a spread and a matrix.
            auto const rest = cursor.rest();
            if (rest.starts_with(spread.name)
                && (rest.size() == spread.name.size() || rest[spread.name.size()] == ' '))
            {
                cursor.advance(spread.name.size());
                gradient.spread = spread.value;
                break;
            }
        }
        cursor.skipSpaces();
        if (cursor.peek() == 'm')
        {
            cursor.advance();
            gradient.transform = readMatrix(cursor, failure);
        }
        cursor.skipSpaces();
        if (!cursor.atEnd())
            failure.fail(VectorGlyphError::Malformed);

        auto last = 0.0F;
        auto stops = firstStop == string_view::npos ? string_view {} : body.substr(firstStop + 1);
        while (!failure.failed() && firstStop != string_view::npos)
        {
            if (gradient.stops.size() == MaxVectorGradientStops)
            {
                failure.fail(VectorGlyphError::TooLarge);
                break;
            }
            auto const end = stops.find('/');
            auto const stop = trim(stops.substr(0, end));
            auto stopCursor = Cursor { stop, failure };
            auto const offset = stopCursor.number();
            // Offsets lie in 0 to 1 and never decrease.
            if (!(offset >= last && offset <= 1.0F) || stopCursor.peek() != ' ')
                failure.fail(VectorGlyphError::Malformed);
            last = offset;
            gradient.stops.push_back(VectorGradientStop {
                .offset = offset, .color = parseSolid(trim(stopCursor.rest()), failure) });
            if (end == string_view::npos)
                break;
            stops.remove_prefix(end + 1);
        }
        if (gradient.stops.size() < 2)
            failure.fail(VectorGlyphError::Malformed);
        return gradient;
    }

    [[nodiscard]] VectorColor parseColor(string_view text, Failure& failure)
    {
        if (text.starts_with("lin("))
            return parseGradient(text.substr(4), GradientKind::Linear, failure);
        if (text.starts_with("rad("))
            return parseGradient(text.substr(4), GradientKind::Radial, failure);
        return parseSolid(text, failure);
    }

    /// A layer's header after its first comma: the colour (or a clip's fill rule), and its matrix.
    struct HeaderValue
    {
        string_view value;
        std::optional<VectorMatrix> transform;
    };

    /// Splits `<value>[,m a b c d e f]`. A gradient's parentheses may hold commas of their own, so its
    /// value runs to the closing parenthesis.
    [[nodiscard]] HeaderValue splitMatrix(string_view rest, Failure& failure) noexcept
    {
        auto const isGradient = rest.starts_with("lin(") || rest.starts_with("rad(");
        auto const closing = rest.find(')');
        if (isGradient && closing == string_view::npos)
        {
            failure.fail(VectorGlyphError::Malformed);
            return {};
        }
        auto const end = isGradient ? closing + 1 : std::min(rest.find(','), rest.size());
        auto const value = trim(rest.substr(0, end));
        auto const tail = trim(rest.substr(end));
        if (tail.empty())
            return { .value = value, .transform = std::nullopt };

        // What follows is `,m` and six numbers, and nothing else.
        auto const matrix = tail.starts_with(',') ? trim(tail.substr(1)) : string_view {};
        if (!matrix.starts_with('m'))
        {
            failure.fail(VectorGlyphError::Malformed);
            return {};
        }
        auto cursor = Cursor { matrix.substr(1), failure };
        auto const transform = readMatrix(cursor, failure);
        cursor.skipSpaces();
        if (!cursor.atEnd())
            failure.fail(VectorGlyphError::Malformed);
        return { .value = value, .transform = transform };
    }

    /// A count in plain decimal digits, four at most.
    [[nodiscard]] size_t parseCount(string_view text, Failure& failure) noexcept
    {
        if (text.empty() || text.size() > 4 || !std::ranges::all_of(text, isDigit))
        {
            failure.fail(VectorGlyphError::Malformed);
            return 0;
        }
        auto count = size_t { 0 };
        for (auto const digit: text)
            count = (count * 10) + static_cast<size_t>(digit - '0');
        return count;
    }

    /// `<width>[/<cap>[/<join>]]`, the part of a stroke's header after `stroke=`.
    [[nodiscard]] VectorStrokeStyle parseStrokeStyle(string_view text,
                                                     double matrixStretch,
                                                     VectorViewBox const& viewBox,
                                                     Failure& failure) noexcept
    {
        auto style = VectorStrokeStyle {};
        auto parts = std::array<string_view, 3> {};
        auto count = size_t { 0 };
        for (auto const part: text | std::views::split('/'))
        {
            if (count == parts.size())
            {
                failure.fail(VectorGlyphError::Malformed);
                return style;
            }
            parts[count++] = string_view { part.begin(), part.end() };
        }

        style.width = Cursor { parts[0], failure }.wholeNumber();
        // A stroke wider than the viewBox only costs time: it covers the whole span either way. Measured in
        // viewBox units, so through the layer's matrix.
        auto const drawnWidth = double { style.width } * matrixStretch;
        if (!(style.width > 0.0F && drawnWidth <= double { std::max(viewBox.width, viewBox.height) }))
            failure.fail(VectorGlyphError::Malformed);

        if (count > 1)
            style.cap = named(LineCaps, parts[1], failure);
        if (count > 2 && parts[2] == "bevel")
            style.join = VectorLineJoin::Bevel;
        else if (count > 2 && parts[2] != "round")
        {
            // `miter`, with its limit right behind it when it is not the default.
            if (!parts[2].starts_with("miter"))
            {
                failure.fail(VectorGlyphError::Malformed);
                return style;
            }
            style.join = VectorLineJoin::Miter;
            auto const limit = parts[2].substr(5);
            if (!limit.empty())
                style.miterLimit = Cursor { limit, failure }.wholeNumber();
            if (!(style.miterLimit >= 1.0F && style.miterLimit <= 1000.0F))
                failure.fail(VectorGlyphError::Malformed);
        }
        return style;
    }

    /// A layer's header: `<paint>,<value>[,m a b c d e f]`.
    void parseHeader(string_view text, VectorViewBox const& viewBox, VectorLayer& layer, Failure& failure)
    {
        text = trim(text);
        auto const comma = text.find(',');
        if (comma == string_view::npos)
        {
            failure.fail(VectorGlyphError::Malformed);
            return;
        }
        auto const paint = trim(text.substr(0, comma));
        auto const header = splitMatrix(trim(text.substr(comma + 1)), failure);
        layer.transform = header.transform;

        if (paint.starts_with("fill="))
            layer.operation = VectorFill { .rule = named(FillRules, paint.substr(5), failure),
                                           .color = parseColor(header.value, failure) };
        else if (paint.starts_with("clip="))
        {
            auto const count = parseCount(paint.substr(5), failure);
            if (count == 0 || count >= MaxVectorLayers)
                failure.fail(VectorGlyphError::Malformed);
            layer.operation = VectorClip { .rule = named(FillRules, header.value, failure), .count = count };
        }
        else if (paint.starts_with("stroke="))
        {
            auto const stretch = layer.transform ? stretchOf(*layer.transform).second : 1.0;
            auto const style = parseStrokeStyle(paint.substr(7), stretch, viewBox, failure);
            layer.operation = VectorStroke { .style = style, .color = parseColor(header.value, failure) };
        }
        else
            failure.fail(VectorGlyphError::Malformed);
    }

    /// Every clip covers layers that exist and are not clips themselves: there is one level of clipping.
    [[nodiscard]] bool clipsAreValid(std::span<VectorLayer const> layers) noexcept
    {
        auto clippedLeft = size_t { 0 };
        for (auto const index: std::views::iota(size_t { 0 }, layers.size()))
        {
            if (auto const* const clip = std::get_if<VectorClip>(&layers[index].operation))
            {
                if (clippedLeft > 0 || index + clip->count >= layers.size())
                    return false;
                clippedLeft = clip->count;
            }
            else if (clippedLeft > 0)
                --clippedLeft;
        }
        return true;
    }

    /// @return Whether @p color, gradient stops included, holds a colour for which @p matches holds.
    template <typename Predicate>
    [[nodiscard]] bool anySolid(VectorColor const& color, Predicate matches)
    {
        if (auto const* const solid = std::get_if<VectorSolid>(&color))
            return matches(*solid);
        return std::ranges::any_of(
            std::get<VectorGradient>(color).stops, matches, &VectorGradientStop::color);
    }

    /// @return The colour a layer paints with; null for a clip.
    [[nodiscard]] VectorColor const* colorOf(VectorLayer const& layer) noexcept
    {
        if (auto const* const fill = std::get_if<VectorFill>(&layer.operation))
            return &fill->color;
        if (auto const* const stroke = std::get_if<VectorStroke>(&layer.operation))
            return &stroke->color;
        return nullptr;
    }
} // namespace

VectorPalette VectorGlyph::palette() const noexcept
{
    auto const isForeground = [](VectorSolid const& solid) {
        return std::holds_alternative<VectorForeground>(solid);
    };
    auto const isFixed = [](VectorSolid const& solid) {
        return std::holds_alternative<VectorFixedColor>(solid);
    };
    auto usesForeground = false;
    auto usesFixed = false;
    for (auto const& layer: layers)
        if (auto const* const color = colorOf(layer))
        {
            usesForeground = usesForeground || anySolid(*color, isForeground);
            usesFixed = usesFixed || anySolid(*color, isFixed);
        }
    if (!usesFixed)
        return VectorPalette::ForegroundOnly;
    return usesForeground ? VectorPalette::Mixed : VectorPalette::FixedOnly;
}

bool isValidVectorViewBox(VectorViewBox const& viewBox) noexcept
{
    auto const inRange = [](float value) {
        return std::abs(value) <= MaxVectorViewBoxValue;
    };
    return inRange(viewBox.x) && inRange(viewBox.y) && inRange(viewBox.width) && inRange(viewBox.height)
           && viewBox.width > 0.0F && viewBox.height > 0.0F;
}

std::optional<VectorViewBox> parseVectorViewBox(std::string_view text)
{
    auto failure = Failure {};
    auto cursor = Cursor { text, failure };
    cursor.skipSpaces();
    auto values = std::array<float, 4> {};
    for (auto const index: std::views::iota(size_t { 0 }, values.size()))
    {
        if (index > 0)
            cursor.skipCommaSpaces();
        values[index] = cursor.number();
    }
    cursor.skipSpaces();
    auto const viewBox =
        VectorViewBox { .x = values[0], .y = values[1], .width = values[2], .height = values[3] };
    if (failure.failed() || !cursor.atEnd() || !isValidVectorViewBox(viewBox))
        return std::nullopt;
    return viewBox;
}

std::expected<VectorGlyph, VectorGlyphError> parseVectorGlyph(std::string_view payload, VectorViewBox viewBox)
{
    auto const isPrintable = [](char ch) {
        return ch >= 0x20 && ch <= 0x7E;
    };
    if (payload.empty() || !std::ranges::all_of(payload, isPrintable))
        return std::unexpected { VectorGlyphError::Malformed };

    auto failure = Failure {};
    auto const bounds = Bounds { viewBox };
    auto budget = Budget { .segments = MaxVectorSegments,
                           .ink = double { std::max(viewBox.width, viewBox.height) } * MaxVectorInkMultiple };
    auto glyph = VectorGlyph { .viewBox = viewBox, .layers = {} };
    for (auto const part: payload | std::views::split('|'))
    {
        if (glyph.layers.size() == MaxVectorLayers)
            return std::unexpected { VectorGlyphError::TooLarge };
        auto const text = string_view { part.begin(), part.end() };
        auto const colon = text.find(':');
        if (colon == string_view::npos)
            return std::unexpected { VectorGlyphError::Malformed };

        auto& layer = glyph.layers.emplace_back();
        parseHeader(text.substr(0, colon), viewBox, layer, failure);
        auto const* const stroke = std::get_if<VectorStroke>(&layer.operation);
        auto const strokeWidth = stroke ? double { stroke->style.width }
                                              * (layer.transform ? stretchOf(*layer.transform).second : 1.0)
                                        : 0.0;
        if (!failure.failed())
            layer.segments =
                parsePathData(text.substr(colon + 1), bounds, budget, strokeWidth, layer.transform, failure);
        if (failure.failed())
            return std::unexpected { failure.error() };
        // Kept for the session.
        layer.segments.shrink_to_fit();
    }
    if (!clipsAreValid(glyph.layers))
        return std::unexpected { VectorGlyphError::Malformed };
    glyph.layers.shrink_to_fit();
    return glyph;
}

} // namespace vtbackend
