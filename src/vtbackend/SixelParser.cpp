/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <vtbackend/SixelParser.h>

#include <algorithm>

#include "vtbackend/primitives.h"

using std::clamp;
using std::fill;
using std::max;
using std::min;
using std::vector;

// VT 340 sixel protocol is defined here: https://vt100.net/docs/vt3xx-gp/chapter14.html

using namespace std;
namespace terminal
{

namespace
{
    constexpr bool isDigit(char value) noexcept
    {
        return value >= '0' && value <= '9';
    }

    constexpr uint8_t toDigit(char value) noexcept
    {
        return static_cast<uint8_t>(value) - '0';
    }

    constexpr bool isSixel(char value) noexcept
    {
        return value >= 63 && value <= 126;
    }

    constexpr int8_t toSixel(char value) noexcept
    {
        return static_cast<int8_t>(static_cast<int>(value) - 63);
    }

    constexpr rgb_color rgb(uint8_t r, uint8_t g, uint8_t b)
    {
        return rgb_color { r, g, b };
    }

    constexpr double hue2rgb(double p, double q, double t) noexcept
    {
        if (t < 0)
            t += 1;
        if (t > 1)
            t -= 1;
        if (t < 1. / 6)
            return p + (q - p) * 6 * t;
        if (t < 1. / 2)
            return q;
        if (t < 2. / 3)
            return p + (q - p) * (2. / 3 - t) * 6;
        return p;
    }

    using normalized_value = double; // normalized value between [0, 1]

    constexpr rgb_color hsl2rgb(normalized_value h, normalized_value s, normalized_value l) noexcept
    {
        // See http://en.wikipedia.org/wiki/HSL_color_space.

        if (0 == s)
        {
            auto const grayscale = static_cast<uint8_t>(l * 255.);
            return rgb_color { grayscale, grayscale, grayscale };
        }
        else
        {
            auto const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
            auto const p = 2 * l - q;

            auto result = rgb_color {};
            result.red = static_cast<uint8_t>(hue2rgb(p, q, h + 1. / 3) * 255);
            result.green = static_cast<uint8_t>(hue2rgb(p, q, h) * 255);
            result.blue = static_cast<uint8_t>(hue2rgb(p, q, h - 1. / 3) * 255);
            return result;
        }
    }

} // namespace

// VT 340 default color palette (https://www.vt100.net/docs/vt3xx-gp/chapter2.html#S2.4)
constexpr inline std::array<rgb_color, 16> defaultColors = {
    rgb(0, 0, 0),       //  0: black
    rgb(51, 51, 204),   //  1: blue
    rgb(204, 33, 33),   //  2: red
    rgb(51, 204, 51),   //  3: green
    rgb(204, 51, 204),  //  4: magenta
    rgb(51, 204, 204),  //  5: cyan
    rgb(204, 204, 51),  //  6: yellow
    rgb(135, 135, 135), //  7: gray 50%
    rgb(66, 66, 66),    //  8: gray 25%
    rgb(84, 84, 153),   //  9: less saturated blue
    rgb(153, 66, 66),   // 10: less saturated red
    rgb(84, 153, 84),   // 11: less saturated green
    rgb(153, 84, 153),  // 12: less saturated magenta
    rgb(84, 153, 153),  // 13: less saturated cyan
    rgb(153, 153, 84),  // 14: less saturated yellow
    rgb(204, 204, 204), // 15: gray 75%
};

// {{{ SixelColorPalette
sixel_color_palette::sixel_color_palette(unsigned int size, unsigned int maxSize): _maxSize { maxSize }
{
    if (size > 0)
        _palette.resize(size);

    reset();
}

void sixel_color_palette::reset()
{
    for (size_t i = 0; i < min(static_cast<size_t>(size()), defaultColors.size()); ++i)
        _palette[i] = defaultColors[i];
}

void sixel_color_palette::setSize(unsigned int newSize)
{
    _palette.resize(static_cast<size_t>(max(0u, min(newSize, _maxSize))));
}

void sixel_color_palette::setColor(unsigned int index, rgb_color const& color)
{
    if (index < _maxSize)
    {
        if (index >= size())
            setSize(index + 1);

        if (static_cast<size_t>(index) < _palette.size())
            _palette.at(index) = color;
    }
}

rgb_color sixel_color_palette::at(unsigned int index) const noexcept
{
    return _palette[index % _palette.size()];
}
// }}}

sixel_parser::sixel_parser(events& events, on_finalize finalizer):
    _events { events }, _finalizer { std::move(finalizer) }
{
}

void sixel_parser::parse(char value)
{
    switch (_state)
    {
        case state::Ground: fallback(value); break;

        case state::RepeatIntroducer:
            // '!' NUMBER BYTE
            if (isDigit(value))
                paramShiftAndAddDigit(toDigit(value));
            else if (isSixel(value))
            {
                auto const sixel = toSixel(value);
                for (unsigned i = 0; i < _params[0]; ++i)
                    _events.render(sixel);
                transitionTo(state::Ground);
            }
            else
                fallback(value);
            break;

        case state::ColorIntroducer:
            if (isDigit(value))
            {
                paramShiftAndAddDigit(toDigit(value));
                transitionTo(state::ColorParam);
            }
            else
                fallback(value);
            break;

        case state::ColorParam:
            if (isDigit(value))
                paramShiftAndAddDigit(toDigit(value));
            else if (value == ';')
                _params.push_back(0);
            else
                fallback(value);
            break;

        case state::RasterSettings:
            if (isDigit(value))
                paramShiftAndAddDigit(toDigit(value));
            else if (value == ';')
                _params.push_back(0);
            else
                fallback(value);
            break;
    }
}

void sixel_parser::fallback(char value)
{
    if (value == '#')
        transitionTo(state::ColorIntroducer);
    else if (value == '!')
        transitionTo(state::RepeatIntroducer);
    else if (value == '"')
        transitionTo(state::RasterSettings);
    else if (value == '$')
    {
        transitionTo(state::Ground);
        _events.rewind();
    }
    else if (value == '-')
    {
        transitionTo(state::Ground);
        _events.newline();
    }
    else
    {
        if (_state != state::Ground)
            transitionTo(state::Ground);

        if (isSixel(value))
            _events.render(toSixel(value));
    }

    // ignore any other input value
}

void sixel_parser::done()
{
    transitionTo(state::Ground); // this also ensures current state's leave action is invoked
    _events.finalize();
    if (_finalizer)
        _finalizer();
}

void sixel_parser::paramShiftAndAddDigit(unsigned value)
{
    unsigned& number = _params.back();
    number = number * 10 + value;
}

void sixel_parser::transitionTo(state newState)
{
    leaveState();
    _state = newState;
    enterState();
}

void sixel_parser::enterState()
{
    switch (_state)
    {
        case state::ColorIntroducer:
        case state::RepeatIntroducer:
        case state::RasterSettings:
            _params.clear();
            _params.push_back(0);
            break;

        case state::Ground:
        case state::ColorParam: break;
    }
}

void sixel_parser::leaveState()
{
    switch (_state)
    {
        case state::Ground:
        case state::ColorIntroducer:
        case state::RepeatIntroducer: break;

        case state::RasterSettings:
            if (_params.size() > 1 && _params.size() < 5)
            {
                auto const pan = _params[0];
                auto const pad = _params[1];

                auto const imageSize =
                    _params.size() > 2
                        ? optional<image_size> { image_size { width(_params[2]), height(_params[3]) } }
                        : std::nullopt;

                _events.setRaster(pan, pad, imageSize);
                _state = state::Ground;
            }
            break;

        case state::ColorParam:
            if (_params.size() == 1)
            {
                auto const index = _params[0];
                _events.useColor(index); // TODO: move color palette into image builder (to have access to it
                                         // during clear!)
            }
            else if (_params.size() == 5)
            {
                auto constexpr convertValue = [](unsigned value) {
                    // converts a color from range 0..100 to 0..255
                    return static_cast<uint8_t>(
                        static_cast<int>((static_cast<float>(value) * 255.0f) / 100.0f) % 256);
                };
                auto const index = _params[0];
                auto const colorSpace = _params[1] == 2 ? colorspace::RGB : colorspace::HSL;
                switch (colorSpace)
                {
                    case colorspace::RGB: {
                        auto const p1 = convertValue(_params[2]);
                        auto const p2 = convertValue(_params[3]);
                        auto const p3 = convertValue(_params[4]);
                        auto const color = rgb_color { p1, p2, p3 }; // TODO: convert HSL if requested
                        _events.setColor(index, color);
                        break;
                    }
                    case colorspace::HSL: {
                        // HLS Values
                        // Px 	0 to 360 degrees 	Hue angle
                        // Py 	0 to 100 percent 	Lightness
                        // Pz 	0 to 100 percent 	Saturation
                        //
                        // (Hue angle seems to be shifted by 120 deg in other Sixel implementations.)
                        auto const h = static_cast<double>(_params[2]) - 120.0;
                        auto const H = (h < 0 ? 360 + h : h) / 360.0;
                        auto const S = static_cast<double>(_params[3]) / 100.0;
                        auto const L = static_cast<double>(_params[3]) / 100.0;
                        auto const rgb = hsl2rgb(H, S, L);
                        _events.setColor(index, rgb);
                        break;
                    }
                }
                _events.useColor(index); // Also use the specified color.
            }
            break;
    }
}

void sixel_parser::pass(char ch)
{
    parse(ch);
}

void sixel_parser::finalize()
{
    done();
}

// =================================================================================

sixel_image_builder::sixel_image_builder(image_size maxSize,
                                         int aspectVertical,
                                         int aspectHorizontal,
                                         rgba_color backgroundColor,
                                         std::shared_ptr<sixel_color_palette> colorPalette):
    _maxSize { maxSize },
    _colors { std::move(colorPalette) },
    _size { image_size { width { 1 }, height { 1 } } },
    _buffer(_maxSize.area() * 4),
    _sixelCursor {},
    _aspectRatio(static_cast<unsigned int>(
        std::ceil(static_cast<float>(aspectVertical) / static_cast<float>(aspectHorizontal)))),
    _sixelBandHeight(6 * _aspectRatio)
{
    clear(backgroundColor);
}

void sixel_image_builder::clear(rgba_color fillColor)
{
    _sixelCursor = {};

    auto* p = _buffer.data();
    auto* const e = p + _maxSize.area() * 4;
    while (p != e)
    {
        *p++ = fillColor.red();
        *p++ = fillColor.green();
        *p++ = fillColor.blue();
        *p++ = fillColor.alpha();
    }
}

rgba_color sixel_image_builder::at(cell_location coord) const noexcept
{
    auto const line = unbox<unsigned>(coord.line) % unbox<unsigned>(_size.height);
    auto const col = unbox<unsigned>(coord.column) % unbox<unsigned>(_size.width);
    auto const base = line * unbox<unsigned>(_size.width) * 4 + col * 4;
    const auto* const color = &_buffer[base];
    return rgba_color { color[0], color[1], color[2], color[3] };
}

void sixel_image_builder::write(cell_location const& coord, rgb_color const& value) noexcept
{
    if (unbox<int>(coord.line) >= 0 && unbox<int>(coord.line) < unbox<int>(_maxSize.height)
        && unbox<int>(coord.column) >= 0 && unbox<int>(coord.column) < unbox<int>(_maxSize.width))
    {
        if (!_explicitSize)
        {
            if (unbox<int>(coord.line) >= unbox<int>(_size.height))
                _size.height = height::cast_from(coord.line.as<unsigned int>() + _aspectRatio);
            if (unbox<int>(coord.column) >= unbox<int>(_size.width))
                _size.width = width::cast_from(coord.column + 1);
        }

        for (unsigned int i = 0; i < _aspectRatio; ++i)
        {
            auto const base = (coord.line.as<unsigned int>() + i)
                                  * unbox<unsigned int>((_explicitSize ? _size.width : _maxSize.width)) * 4u
                              + unbox<unsigned int>(coord.column) * 4u;
            _buffer[base + 0] = value.red;
            _buffer[base + 1] = value.green;
            _buffer[base + 2] = value.blue;
            _buffer[base + 3] = 0xFF;
        }
    }
}

void sixel_image_builder::setColor(unsigned index, rgb_color const& color)
{
    _colors->setColor(index, color);
}

void sixel_image_builder::useColor(unsigned index)
{
    _currentColor = index % _colors->size();
}

void sixel_image_builder::rewind()
{
    _sixelCursor.column = {};
}

void sixel_image_builder::newline()
{
    _sixelCursor.column = {};
    if (unbox<unsigned int>(_sixelCursor.line) + _sixelBandHeight
        < unbox<unsigned int>(_explicitSize ? _size.height : _maxSize.height))
        _sixelCursor.line = line_offset::cast_from(_sixelCursor.line.as<unsigned int>() + _sixelBandHeight);
}

void sixel_image_builder::setRaster(unsigned int pan, unsigned int pad, optional<image_size> imageSize)
{
    if (pad != 0)
        _aspectRatio =
            max(1u, static_cast<unsigned int>(std::ceil(static_cast<float>(pan) / static_cast<float>(pad))));
    _sixelBandHeight = 6 * _aspectRatio;
    if (imageSize)
    {
        imageSize->height = height::cast_from(imageSize->height.value * _aspectRatio);
        _size.width = clamp(imageSize->width, width(0), _maxSize.width);
        _size.height = clamp(imageSize->height, height(0), _maxSize.height);
        _buffer.resize(_size.area() * 4);
        _explicitSize = true;
    }
}

void sixel_image_builder::render(int8_t sixel)
{
    // TODO: respect aspect ratio!
    auto const x = _sixelCursor.column;
    if (unbox<int>(x) < unbox<int>((_explicitSize ? _size.width : _maxSize.width)))
    {
        for (unsigned int i = 0; i < 6; ++i)
        {
            auto const y = _sixelCursor.line + static_cast<int>(i * _aspectRatio);
            auto const pos = cell_location { y, x };
            auto const pin = 1 << i;
            auto const pinned = (sixel & pin) != 0;
            if (pinned)
                write(pos, currentColor());
        }
        _sixelCursor.column++;
    }
}

void sixel_image_builder::finalize()
{
    if (unbox<int>(_size.height) == 1)
    {
        _size.height = height::cast_from(_sixelCursor.line.as<unsigned int>() * _aspectRatio);
        _buffer.resize(_size.area() * 4);
        return;
    }
    if (!_explicitSize)
    {
        buffer tempBuffer(static_cast<size_t>(_size.height.value * _size.width.value) * 4);
        for (auto i = 0; i < unbox<int>(_size.height); ++i)
        {
            for (auto j = 0; j < unbox<int>(_size.width); ++j)
            {
                std::copy_n(_buffer.begin() + i * unbox<long>(_maxSize.width) * 4,
                            _size.width.value * 4,
                            tempBuffer.begin() + i * unbox<long>(_size.width) * 4);
            }
        }
        _buffer.swap(tempBuffer);
        _explicitSize = false;
    }
}

} // namespace terminal
