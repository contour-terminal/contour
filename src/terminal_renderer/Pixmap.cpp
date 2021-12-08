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
#include <terminal_renderer/Pixmap.h>
#include <terminal_renderer/utils.h>

using std::clamp;
using std::min;
using std::max;
using std::move;
using std::swap;

namespace terminal::renderer
{

namespace
{
    struct From { int value; };
    struct To { int value; };
    struct BaseOffset { int value; };
    enum class Orientation { Horizontal, Vertical };

    Pixmap& segment_line(Pixmap& pixmap, Orientation orientation, BaseOffset base, From from, To to)
    {
        // If the font size is very very small, line length calculation might cause negative values.
        // Be sensible here then to not cause an infinite loop.
        to.value = max(from.value, to.value);

        switch (orientation)
        {
            case Orientation::Horizontal:
                for (auto const y: ranges::views::iota(base.value - 1, base.value + 1))
                    for (auto const x: ranges::views::iota(from.value, to.value))
                        pixmap.paint(x, y);
                break;
            case Orientation::Vertical:
                for (auto const y: ranges::views::iota(from.value, to.value))
                    for (auto const x: ranges::views::iota(base.value - 1, base.value + 1))
                        pixmap.paint(x, y);
                break;
        }
        return pixmap;
    }
}

atlas::Buffer Pixmap::take()
{
    if (_size != _downsampledSize)
        return downsample(_buffer, 1, _size, _downsampledSize);
    else
        return move(_buffer);
}

Pixmap& Pixmap::line(Ratio _from, Ratio _to)
{
    if (_from.y > _to.y)
        swap(_from, _to);
    auto const from = _size * _from;
    auto const to = _size * _to;
    auto const z = max(1, _lineThickness / 2);
    if (from.x != to.x)
    {
        auto const f = linearEq(from, to);
        for (auto const x: ranges::views::iota(0, unbox<int>(_size.width)))
            if (auto const y = f(x); from.y <= y && y <= to.y)
                for (auto const i: ranges::views::iota(-z, z))
                    paint(x, y + i);
    }
    else
    {
        for (auto const y: ranges::views::iota(from.y, to.y))
            for (auto const i: ranges::views::iota(-z, z))
                paint(from.x, y + i);
    }
    return *this;
}

Pixmap& Pixmap::halfFilledCircleLeft()
{
    auto const w = unbox<int>(_size.width);
    auto const h = unbox<int>(_size.height);
    auto const putpixel = [&](int x, int y)
    {
        auto const xf = clamp(x, 0, w - 1);
        auto const yf = clamp(y, 0, h - 1);
        for (int xi = xf; xi < w; ++xi)
            paint(xi, yf, 0xFF);
    };
    auto const putAbove = [&](int x, int y) { putpixel(x, y - h/2); };
    auto const putBelow = [&](int x, int y) { putpixel(x, y + h/2); };
    auto const radius   = crispy::Point{ w, h / 2 };
    drawEllipseArc(putAbove, _size, radius, Arc::BottomLeft);
    drawEllipseArc(putBelow, _size, radius, Arc::TopLeft);
    return *this;
}

Pixmap& Pixmap::halfFilledCircleRight()
{
    auto const w = unbox<int>(_size.width);
    auto const h = unbox<int>(_size.height);
    auto const putpixel = [&](int x, int y)
    {
        auto const fx = min(w - 1, x);
        for (int x = 0; x < fx; ++x)
            paint(x, y, 0xFF);
    };
    auto const putAbove = [&](int x, int y) { putpixel(x, y - h/2); };
    auto const putBelow = [&](int x, int y) { putpixel(x, y + h/2); };
    auto const radius   = crispy::Point{ w, h / 2 };
    drawEllipseArc(putAbove, _size, radius, Arc::BottomRight);
    drawEllipseArc(putBelow, _size, radius, Arc::TopRight);
    return *this;
}

Pixmap& Pixmap::segment_bar(int which)
{
    assert(1 <= which && which <= 7);

    //   --1--
    //  4     2
    //   --3--
    //  7     5
    //   --6--

    auto const Z = _lineThickness;

    auto const L = 2 * Z;
    auto const R = unbox<int>(_size.width) - Z;

    auto const T = static_cast<int>(ceil(unbox<double>(_size.height) * 1/8_th)); // Z;
    auto const B = unbox<int>(_size.height) - _baseLine - Z / 2;
    auto const M = T + (B - T) / 2;

    switch (which)
    {
        case 1: return segment_line(*this, Orientation::Horizontal, BaseOffset{T}, From{L},   To{R});
        case 2: return segment_line(*this, Orientation::Vertical,   BaseOffset{R}, From{T+Z}, To{M-Z});
        case 3: return segment_line(*this, Orientation::Horizontal, BaseOffset{M}, From{L},   To{R});
        case 4: return segment_line(*this, Orientation::Vertical,   BaseOffset{L}, From{T+Z}, To{M-Z});
        case 5: return segment_line(*this, Orientation::Vertical,   BaseOffset{R}, From{M+Z}, To{B-Z});
        case 6: return segment_line(*this, Orientation::Horizontal, BaseOffset{B}, From{L},   To{R});
        case 7: return segment_line(*this, Orientation::Vertical,   BaseOffset{L}, From{M+Z}, To{B-Z});
    }

    assert(false);
    return *this;
}

} // end namespace terminal::renderer
