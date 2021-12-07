/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <terminal_renderer/Atlas.h>
#include <terminal/primitives.h>

#include <range/v3/view/iota.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <functional>

namespace terminal::renderer
{

// Helper to write ratios like 1/8_th
struct Ratio1 { double value; };
constexpr Ratio1 operator "" _th(unsigned long long ratio) { return Ratio1{static_cast<double>(ratio)}; }
constexpr double operator/(int a, Ratio1 b) noexcept { return static_cast<double>(a) / b.value; }

// ratio between 0.0 and 1.0 for x (horizontal) and y (vertical).
struct Ratio { double x; double y; };

struct RatioBlock { Ratio from{}; Ratio to{}; };
constexpr RatioBlock lower(double r) noexcept { return RatioBlock{{0, 1 - r}, {1, 1}}; }
constexpr RatioBlock upper(double r) noexcept { return RatioBlock{{0, 0}, {1, r}}; }
constexpr RatioBlock left(double r)  noexcept { return RatioBlock{{0, 0}, {r, 1}}; }
constexpr RatioBlock right(double r) noexcept { return RatioBlock{{1.f - r, 0.f}, {1.f, 1.f}}; }

constexpr crispy::Point operator*(ImageSize a, Ratio b) noexcept
{
    return crispy::Point{
        static_cast<int>(a.width.as<double>() * b.x),
        static_cast<int>(a.height.as<double>() * b.y)
    };
}

constexpr auto linearEq(crispy::Point p1, crispy::Point p2) noexcept
{
    // Expects(p2.x != p1.x);
    auto const m = double(p2.y - p1.y) / double(p2.x - p1.x);
    auto const n = double(p1.y) - m * double(p1.x);
    return [m, n](int x) -> int { return int(double(m) * double(x) + n); };
}

enum class Dir { Top, Right, Bottom, Left };
enum class Inverted { No, Yes };

enum Arc
{
    NoArc, TopLeft, TopRight, BottomRight, BottomLeft
};

template <typename F>
auto makeDraw4WaySymmetric(Arc arc, ImageSize size, F putpixel)
{
    return [=](int x, int y)
    {
        auto const w = unbox<int>(size.width);
        auto const h = unbox<int>(size.height);
        switch (arc)
        {
            case Arc::TopLeft:     putpixel(w - x,     y); break;
            case Arc::TopRight:    putpixel(    x,     y); break;
            case Arc::BottomLeft:  putpixel(w - x, h - y); break;
            case Arc::BottomRight: putpixel(    x, h - y); break;
            case Arc::NoArc:       break;
        }
    };
}

template <typename F>
constexpr void drawEllipse(F doDraw4WaySymmetric, crispy::Point radius)
{
    auto const rx = radius.x;
    auto const ry = radius.y;

    double dx{}, dy{}, d1{}, d2{};
    double x {0};
    double y {static_cast<double>(ry)};

    // Initial decision parameter of region 1
    d1 = (ry * ry) - (rx * rx * ry) +
                     (0.25 * rx * rx);
    dx = 2 * ry * ry * x;
    dy = 2 * rx * rx * y;

    while (dx < dy)
    {
        doDraw4WaySymmetric(x, y);

        // Checking and updating value of decision parameter based on algorithm
        if (d1 < 0)
        {
            x++;
            dx = dx + (2 * ry * ry);
            d1 = d1 + dx + (ry * ry);
        }
        else
        {
            x++;
            y--;
            dx = dx + (2 * ry * ry);
            dy = dy - (2 * rx * rx);
            d1 = d1 + dx - dy + (ry * ry);
        }
    }

    // Decision parameter of region 2
    d2 = ((ry * ry) * ((x + 0.5) * (x + 0.5))) +
         ((rx * rx) * ((y - 1) * (y - 1))) -
          (rx * rx * ry * ry);

    while (y >= 0)
    {
        doDraw4WaySymmetric(x, y);

        // Checking and updating parameter value based on algorithm
        if (d2 > 0)
        {
            y--;
            dy = dy - (2 * rx * rx);
            d2 = d2 + (rx * rx) - dy;
        }
        else
        {
            y--;
            x++;
            dx = dx + (2 * ry * ry);
            dy = dy - (2 * rx * rx);
            d2 = d2 + dx - dy + (rx * rx);
        }
    }
}

template <typename PutPixel>
constexpr void drawEllipseArc(PutPixel putpixel,
                              ImageSize imageSize,
                              crispy::Point radius,
                              Arc arc)
{
    drawEllipse(makeDraw4WaySymmetric(arc, imageSize, std::move(putpixel)), radius);
}

/// Alpha-channel 2D image.
struct Pixmap
{
    atlas::Buffer _buffer{};
    ImageSize _size{};
    ImageSize _downsampledSize{};
    std::function<int(int, int)> _filler = [](int, int) { return 0xFF; };
    int _lineThickness = 1;
    int _baseLine = 0;  // baseline position relative to cell bottom.

    constexpr ImageSize downsampledSize() const noexcept { return _downsampledSize; }

    Pixmap& halfFilledCircleLeft();
    Pixmap& halfFilledCircleRight();
    Pixmap& lineThickness(int n) noexcept { _lineThickness = n; return *this; }
    Pixmap& baseline(int n) noexcept { _baseLine = n; return *this; }
    Pixmap& line(Ratio _from, Ratio _to);

    Pixmap& segment_bar(int which);
    template <typename... More> Pixmap& segment_bar(int which, More... more);

    Pixmap& fill() { return fill(_filler); }
    template <typename F> Pixmap& fill(F const& filler);

    void paint(int x, int y, uint8_t value = 0xFF);
    void paintOver(int x, int y, uint8_t intensity);

    atlas::Buffer take();

    operator atlas::Buffer () { return take(); }
};

template <std::size_t SupersamplingFactor = 1>
inline Pixmap blockElement(ImageSize size)
{
    auto const superSize = size * SupersamplingFactor;
    return Pixmap{
        atlas::Buffer(superSize.width.as<size_t>() * superSize.height.as<size_t>(), 0x00),
        superSize,
        size
    };
}

template <size_t N, typename F>
Pixmap blockElement(ImageSize size, F f)
{
    auto p = blockElement<N>(size);
    p._filler = f;
    return p;
}

// {{{ Pixmap inlines
inline void Pixmap::paint(int x, int y, uint8_t _value)
{
    auto const w = unbox<int>(_size.width);
    auto const h = unbox<int>(_size.height) - 1;
    if (!(0 <= y && y <= h))
        return;
    if (!(0 <= x && x < w))
        return;
    _buffer.at((h - y) * w + x) = _value;
}

inline void Pixmap::paintOver(int x, int y, uint8_t intensity)
{
    auto const w = unbox<int>(_size.width);
    auto const h = unbox<int>(_size.height) - 1;
    if (!(0 <= y && y <= h))
        return;
    if (!(0 <= x && x < w))
        return;
    auto& target = _buffer.at((h - y) * w + x);
    target = static_cast<uint8_t>(std::min(
        static_cast<int>(target) + static_cast<int>(intensity),
        255
    ));
}

template <typename F>
Pixmap& Pixmap::fill(F const& filler)
{
    auto const h = unbox<int>(_size.height) - 1;
    for (auto const y: ranges::views::iota(0, unbox<int>(_size.height)))
        for (auto const x: ranges::views::iota(0, unbox<int>(_size.width)))
            _buffer[(h - y) * *_size.width + x] = filler(x, y);
    return *this;
}

template <typename... More>
Pixmap& Pixmap::segment_bar(int which, More... more)
{
    segment_bar(which);
    return segment_bar(more...);
}
// }}}

} // end namespace terminal::renderer

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::renderer::Arc> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }
        template <typename FormatContext>
        auto format(terminal::renderer::Arc value, FormatContext& ctx)
        {
            using terminal::renderer::Arc;
            switch (value)
            {
                case Arc::NoArc: return format_to(ctx.out(), "NoArc");
                case Arc::TopLeft: return format_to(ctx.out(), "TopLeft");
                case Arc::TopRight: return format_to(ctx.out(), "TopRight");
                case Arc::BottomLeft: return format_to(ctx.out(), "BottomLeft");
                case Arc::BottomRight: return format_to(ctx.out(), "BottomRight");
            }
            return format_to(ctx.out(), "?");
        }
    };
} // }}}

