// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <vtrasterizer/TextureAtlas.h> // Buffer

#include <crispy/point.h>

#include <fmt/format.h>

#include <range/v3/view/iota.hpp>

#include <algorithm>
#include <functional>

namespace terminal::rasterizer
{

// Helper to write ratios like 1/8_th
struct Ratio1
{
    double value;
};
constexpr Ratio1 operator"" _th(unsigned long long ratio)
{
    return Ratio1 { static_cast<double>(ratio) };
}
constexpr double operator/(int a, Ratio1 b) noexcept
{
    return static_cast<double>(a) / b.value;
}

// ratio between 0.0 and 1.0 for x (horizontal) and y (vertical).
struct Ratio
{
    double x;
    double y;
};

// clang-format off
struct RatioBlock { Ratio from {}; Ratio to {}; };
constexpr RatioBlock lower(double r) noexcept { return RatioBlock { { 0, 1 - r },     { 1, 1 } }; }
constexpr RatioBlock upper(double r) noexcept { return RatioBlock { { 0, 0 },         { 1, r } }; }
constexpr RatioBlock left(double r) noexcept  { return RatioBlock { { 0, 0 },         { r, 1 } }; }
constexpr RatioBlock right(double r) noexcept { return RatioBlock { { 1.f - r, 0.f }, { 1.f, 1.f } }; }
// clang-format on

constexpr crispy::point operator*(ImageSize a, Ratio b) noexcept
{
    return crispy::point { static_cast<int>(a.width.as<double>() * b.x),
                           static_cast<int>(a.height.as<double>() * b.y) };
}

constexpr auto linearEq(crispy::point p1, crispy::point p2) noexcept
{
    // Require(p2.x != p1.x);
    auto const m = double(p2.y - p1.y) / double(p2.x - p1.x);
    auto const n = double(p1.y) - m * double(p1.x);
    return [m, n](int x) -> int {
        return int(double(m) * double(x) + n);
    };
}

enum class Dir
{
    Top,
    Right,
    Bottom,
    Left
};
enum class Inverted
{
    No,
    Yes
};

enum Arc
{
    NoArc,
    TopLeft,
    TopRight,
    BottomRight,
    BottomLeft
};

template <typename F>
auto makeDraw4WaySymmetric(Arc arc, ImageSize size, F putpixel)
{
    return [=](int x, int y) {
        auto const w = unbox<int>(size.width);
        auto const h = unbox<int>(size.height);
        switch (arc)
        {
            case Arc::TopLeft: putpixel(w - x, y); break;
            case Arc::TopRight: putpixel(x, y); break;
            case Arc::BottomLeft: putpixel(w - x, h - y); break;
            case Arc::BottomRight: putpixel(x, h - y); break;
            case Arc::NoArc: break;
        }
    };
}

template <typename F>
constexpr void drawEllipse(F doDraw4WaySymmetric, crispy::point radius)
{
    auto const rx = radius.x;
    auto const ry = radius.y;

    double dx {};
    double dy {};
    double d1 {};
    double d2 {};
    double x { 0 };
    double y { static_cast<double>(ry) };

    // Initial decision parameter of region 1
    d1 = (ry * ry) - (rx * rx * ry) + (0.25 * rx * rx);
    dx = 2 * ry * ry * x;
    dy = 2 * rx * rx * y;

    while (dx < dy)
    {
        doDraw4WaySymmetric(int(x), int(y));

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
    d2 = ((ry * ry) * ((x + 0.5) * (x + 0.5))) + ((rx * rx) * ((y - 1) * (y - 1))) - (rx * rx * ry * ry);

    while (y >= 0)
    {
        doDraw4WaySymmetric(int(x), int(y));

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
constexpr void drawEllipseArc(PutPixel putpixel, ImageSize imageSize, crispy::point radius, Arc arc)
{
    drawEllipse(makeDraw4WaySymmetric(arc, imageSize, std::move(putpixel)), radius);
}

/// Alpha-channel 2D image.
struct Pixmap
{
    atlas::Buffer buffer {};
    ImageSize size {};
    ImageSize downsampledSize {};
    std::function<int(int, int)> filler = [](int, int) {
        return 0xFF;
    };
    int lineThickness = 1;
    int baseLine = 0; // baseline position relative to cell bottom.

    Pixmap& halfFilledCircleLeft();
    Pixmap& halfFilledCircleRight();
    Pixmap& getlineThickness(int n) noexcept
    {
        lineThickness = n;
        return *this;
    }
    Pixmap& baseline(int n) noexcept
    {
        baseLine = n;
        return *this;
    }
    Pixmap& line(Ratio from, Ratio to);

    Pixmap& rect(Ratio topLeft, Ratio bottomRight) noexcept
    {
        auto const top = int(topLeft.y * unbox<double>(size.height));
        auto const left = int(topLeft.x * unbox<double>(size.width));
        auto const bottom = int(bottomRight.y * unbox<double>(size.height));
        auto const right = int(bottomRight.x * unbox<double>(size.width));

        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x)
                paint(x, y, 0xFF);

        return *this;
    }

    Pixmap& segment_bar(int which);
    template <typename... More>
    Pixmap& segment_bar(int which, More... more);

    Pixmap& fill() { return fill(filler); }
    template <typename F>
    Pixmap& fill(F const& filler);

    void paint(int x, int y, uint8_t value = 0xFF);
    void paintOver(int x, int y, uint8_t intensity);
    void paintOverThick(int x, int y, uint8_t intensity, int sx, int sy);

    atlas::Buffer take();

    operator atlas::Buffer() { return take(); }
};

template <std::size_t SupersamplingFactor = 1>
inline Pixmap blockElement(ImageSize size)
{
    auto const superSize = size * SupersamplingFactor;
    return Pixmap { atlas::Buffer(superSize.width.as<size_t>() * superSize.height.as<size_t>(), 0x00),
                    superSize,
                    size };
}

template <size_t N, typename F>
Pixmap blockElement(ImageSize size, F f)
{
    auto p = blockElement<N>(size);
    p.filler = f;
    return p;
}

// {{{ Pixmap inlines
inline void Pixmap::paint(int x, int y, uint8_t value)
{
    auto const w = unbox<int>(size.width);
    auto const h = unbox<int>(size.height) - 1;
    if (!(0 <= y && y <= h))
        return;
    if (!(0 <= x && x < w))
        return;
    buffer.at(static_cast<unsigned>((h - y) * w + x)) = value;
}

inline void Pixmap::paintOver(int x, int y, uint8_t intensity)
{
    auto const w = unbox<int>(size.width);
    auto const h = unbox<int>(size.height) - 1;
    if (!(0 <= y && y <= h))
        return;
    if (!(0 <= x && x < w))
        return;
    auto& target = buffer.at(static_cast<unsigned>((h - y) * w + x));
    target = static_cast<uint8_t>(std::min(static_cast<int>(target) + static_cast<int>(intensity), 255));
}

inline void Pixmap::paintOverThick(int x, int y, uint8_t intensity, int sx, int sy)
{
    for (int i = x - sx; i <= x + sx; ++i)
        for (int j = y - sy; j <= y + sy; ++j)
            paintOver(i, j, intensity);
}

template <typename F>
Pixmap& Pixmap::fill(F const& filler)
{
    for (auto const y: ::ranges::views::iota(0, unbox<int>(size.height)))
        for (auto const x: ::ranges::views::iota(0, unbox<int>(size.width)))
            paint(x, y, static_cast<uint8_t>(filler(x, y)));
    return *this;
}

template <typename... More>
Pixmap& Pixmap::segment_bar(int which, More... more)
{
    segment_bar(which);
    return segment_bar(more...);
}
// }}}

} // end namespace terminal::rasterizer

template <>
struct fmt::formatter<terminal::rasterizer::Arc>: fmt::formatter<string_view>
{
    auto format(terminal::rasterizer::Arc value, format_context& ctx) -> format_context::iterator
    {
        using terminal::rasterizer::Arc;
        string_view name;
        switch (value)
        {
            case Arc::NoArc: name = "NoArc"; break;
            case Arc::TopLeft: name = "TopLeft"; break;
            case Arc::TopRight: name = "TopRight"; break;
            case Arc::BottomLeft: name = "BottomLeft"; break;
            case Arc::BottomRight: name = "BottomRight"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
