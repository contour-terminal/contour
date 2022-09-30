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
#include <terminal/SixelParser.h>

#include <algorithm>

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
    constexpr bool isDigit(char _value) noexcept
    {
        return _value >= '0' && _value <= '9';
    }

    constexpr uint8_t toDigit(char _value) noexcept
    {
        return static_cast<uint8_t>(_value) - '0';
    }

    constexpr bool isSixel(char _value) noexcept
    {
        return _value >= 63 && _value <= 126;
    }

    constexpr int8_t toSixel(char _value) noexcept
    {
        return static_cast<int8_t>(static_cast<int>(_value) - 63);
    }

    constexpr RGBColor rgb(uint8_t r, uint8_t g, uint8_t b)
    {
        return RGBColor { r, g, b };
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

    using NormalizedValue = double; // normalized value between [0, 1]

    constexpr RGBColor hsl2rgb(NormalizedValue h, NormalizedValue s, NormalizedValue l) noexcept
    {
        // See http://en.wikipedia.org/wiki/HSL_color_space.

        if (0 == s)
        {
            auto const grayscale = static_cast<uint8_t>(l * 255.);
            return RGBColor { grayscale, grayscale, grayscale };
        }
        else
        {
            auto const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
            auto const p = 2 * l - q;

            auto result = RGBColor {};
            result.red = static_cast<uint8_t>(hue2rgb(p, q, h + 1. / 3) * 255);
            result.green = static_cast<uint8_t>(hue2rgb(p, q, h) * 255);
            result.blue = static_cast<uint8_t>(hue2rgb(p, q, h - 1. / 3) * 255);
            return result;
        }
    }

} // namespace

// VT 340 default color palette (https://www.vt100.net/docs/vt3xx-gp/chapter2.html#S2.4)
constexpr inline std::array<RGBColor, 16> defaultColors = {
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
SixelColorPalette::SixelColorPalette(unsigned int _size, unsigned int _maxSize): maxSize_ { _maxSize }
{
    if (_size > 0)
        palette_.resize(_size);

    reset();
}

void SixelColorPalette::reset()
{
    for (size_t i = 0; i < min(static_cast<size_t>(size()), defaultColors.size()); ++i)
        palette_[i] = defaultColors[i];
}

void SixelColorPalette::setSize(unsigned int _newSize)
{
    palette_.resize(static_cast<size_t>(max(0u, min(_newSize, maxSize_))));
}

void SixelColorPalette::setMaxSize(unsigned int _value)
{
    maxSize_ = _value;
}

void SixelColorPalette::setColor(unsigned int _index, RGBColor const& _color)
{
    if (_index < maxSize_)
    {
        if (_index >= size())
            setSize(_index + 1);

        if (static_cast<size_t>(_index) < palette_.size())
            palette_.at(_index) = _color;
    }
}

RGBColor SixelColorPalette::at(unsigned int _index) const noexcept
{
    return palette_[_index % palette_.size()];
}
// }}}

SixelParser::SixelParser(Events& _events, OnFinalize _finalizer):
    events_ { _events }, finalizer_ { std::move(_finalizer) }
{
}

void SixelParser::parse(char _value)
{
    switch (state_)
    {
        case State::Ground: fallback(_value); break;

        case State::RepeatIntroducer:
            // '!' NUMBER BYTE
            if (isDigit(_value))
                paramShiftAndAddDigit(toDigit(_value));
            else if (isSixel(_value))
            {
                auto const sixel = toSixel(_value);
                for (unsigned i = 0; i < params_[0]; ++i)
                    events_.render(sixel);
                transitionTo(State::Ground);
            }
            else
                fallback(_value);
            break;

        case State::ColorIntroducer:
            if (isDigit(_value))
            {
                paramShiftAndAddDigit(toDigit(_value));
                transitionTo(State::ColorParam);
            }
            else
                fallback(_value);
            break;

        case State::ColorParam:
            if (isDigit(_value))
                paramShiftAndAddDigit(toDigit(_value));
            else if (_value == ';')
                params_.push_back(0);
            else
                fallback(_value);
            break;

        case State::RasterSettings:
            if (isDigit(_value))
                paramShiftAndAddDigit(toDigit(_value));
            else if (_value == ';')
                params_.push_back(0);
            else
                fallback(_value);
            break;
    }
}

void SixelParser::fallback(char _value)
{
    if (_value == '#')
        transitionTo(State::ColorIntroducer);
    else if (_value == '!')
        transitionTo(State::RepeatIntroducer);
    else if (_value == '"')
        transitionTo(State::RasterSettings);
    else if (_value == '$')
    {
        transitionTo(State::Ground);
        events_.rewind();
    }
    else if (_value == '-')
    {
        transitionTo(State::Ground);
        events_.newline();
    }
    else
    {
        if (state_ != State::Ground)
            transitionTo(State::Ground);

        if (isSixel(_value))
            events_.render(toSixel(_value));
    }

    // ignore any other input value
}

void SixelParser::done()
{
    transitionTo(State::Ground); // this also ensures current state's leave action is invoked
    events_.finalize();
    if (finalizer_)
        finalizer_();
}

void SixelParser::paramShiftAndAddDigit(unsigned _value)
{
    unsigned& number = params_.back();
    number = number * 10 + _value;
}

void SixelParser::transitionTo(State _newState)
{
    leaveState();
    state_ = _newState;
    enterState();
}

void SixelParser::enterState()
{
    switch (state_)
    {
        case State::ColorIntroducer:
        case State::RepeatIntroducer:
        case State::RasterSettings:
            params_.clear();
            params_.push_back(0);
            break;

        case State::Ground:
        case State::ColorParam: break;
    }
}

void SixelParser::leaveState()
{
    switch (state_)
    {
        case State::Ground:
        case State::ColorIntroducer:
        case State::RepeatIntroducer: break;

        case State::RasterSettings:
            if (params_.size() > 1 && params_.size() < 5)
            {
                auto const pan = params_[0];
                auto const pad = params_[1];

                auto const imageSize =
                    params_.size() > 2
                        ? optional<ImageSize> { ImageSize { Width(params_[2]), Height(params_[3]) } }
                        : std::nullopt;

                events_.setRaster(pan, pad, imageSize);
                state_ = State::Ground;
            }
            break;

        case State::ColorParam:
            if (params_.size() == 1)
            {
                auto const index = params_[0];
                events_.useColor(index); // TODO: move color palette into image builder (to have access to it
                                         // during clear!)
            }
            else if (params_.size() == 5)
            {
                auto constexpr convertValue = [](unsigned _value) {
                    // converts a color from range 0..100 to 0..255
                    return static_cast<uint8_t>(
                        static_cast<int>((static_cast<float>(_value) * 255.0f) / 100.0f) % 256);
                };
                auto const index = params_[0];
                auto const colorSpace = params_[1] == 2 ? Colorspace::RGB : Colorspace::HSL;
                switch (colorSpace)
                {
                    case Colorspace::RGB: {
                        auto const p1 = convertValue(params_[2]);
                        auto const p2 = convertValue(params_[3]);
                        auto const p3 = convertValue(params_[4]);
                        auto const color = RGBColor { p1, p2, p3 }; // TODO: convert HSL if requested
                        events_.setColor(index, color);
                        break;
                    }
                    case Colorspace::HSL: {
                        // HLS Values
                        // Px 	0 to 360 degrees 	Hue angle
                        // Py 	0 to 100 percent 	Lightness
                        // Pz 	0 to 100 percent 	Saturation
                        //
                        // (Hue angle seems to be shifted by 120 deg in other Sixel implementations.)
                        auto const h = static_cast<double>(params_[2]) - 120.0;
                        auto const H = (h < 0 ? 360 + h : h) / 360.0;
                        auto const S = static_cast<double>(params_[3]) / 100.0;
                        auto const L = static_cast<double>(params_[3]) / 100.0;
                        auto const rgb = hsl2rgb(H, S, L);
                        events_.setColor(index, rgb);
                        break;
                    }
                }
                events_.useColor(index); // Also use the specified color.
            }
            break;
    }
}

void SixelParser::pass(char _char)
{
    parse(_char);
}

void SixelParser::finalize()
{
    done();
}

// =================================================================================

SixelImageBuilder::SixelImageBuilder(ImageSize _maxSize,
                                     int _aspectVertical,
                                     int _aspectHorizontal,
                                     RGBAColor _backgroundColor,
                                     std::shared_ptr<SixelColorPalette> _colorPalette):
    maxSize_ { _maxSize },
    colors_ { std::move(_colorPalette) },
    size_ { ImageSize { Width { 1 }, Height { 1 } } },
    buffer_(maxSize_.area() * 4),
    sixelCursor_ {},
    currentColor_ { 0 },
    aspectRatio_(static_cast<unsigned int>(
        std::ceil(static_cast<float>(_aspectVertical) / static_cast<float>(_aspectHorizontal)))),
    sixelBandHeight_(6 * aspectRatio_)
{
    clear(_backgroundColor);
}

void SixelImageBuilder::clear(RGBAColor _fillColor)
{
    sixelCursor_ = {};

    auto p = buffer_.data();
    auto const e = p + maxSize_.area() * 4;
    while (p != e)
    {
        *p++ = _fillColor.red();
        *p++ = _fillColor.green();
        *p++ = _fillColor.blue();
        *p++ = _fillColor.alpha();
    }
}

RGBAColor SixelImageBuilder::at(CellLocation _coord) const noexcept
{
    auto const line = unbox<unsigned>(_coord.line) % unbox<unsigned>(size_.height);
    auto const col = unbox<unsigned>(_coord.column) % unbox<unsigned>(size_.width);
    auto const base = line * unbox<unsigned>(size_.width) * 4 + col * 4;
    auto const color = &buffer_[base];
    return RGBAColor { color[0], color[1], color[2], color[3] };
}

void SixelImageBuilder::write(CellLocation const& _coord, RGBColor const& _value) noexcept
{
    if (unbox<int>(_coord.line) >= 0 && unbox<int>(_coord.line) < unbox<int>(maxSize_.height)
        && unbox<int>(_coord.column) >= 0 && unbox<int>(_coord.column) < unbox<int>(maxSize_.width))
    {
        if (!explicitSize_)
        {
            if (unbox<int>(_coord.line) >= unbox<int>(size_.height))
                size_.height = Height::cast_from(_coord.line.as<unsigned int>() + aspectRatio_);
            if (unbox<int>(_coord.column) >= unbox<int>(size_.width))
                size_.width = Width::cast_from(_coord.column + 1);
        }

        for (unsigned int i = 0; i < aspectRatio_; ++i)
        {
            auto const base = (_coord.line.as<unsigned int>() + i)
                                  * unbox<unsigned int>((explicitSize_ ? size_.width : maxSize_.width)) * 4u
                              + unbox<unsigned int>(_coord.column) * 4u;
            buffer_[base + 0] = _value.red;
            buffer_[base + 1] = _value.green;
            buffer_[base + 2] = _value.blue;
            buffer_[base + 3] = 0xFF;
        }
    }
}

void SixelImageBuilder::setColor(unsigned _index, RGBColor const& _color)
{
    colors_->setColor(_index, _color);
}

void SixelImageBuilder::useColor(unsigned _index)
{
    currentColor_ = _index % colors_->size();
}

void SixelImageBuilder::rewind()
{
    sixelCursor_.column = {};
}

void SixelImageBuilder::newline()
{
    sixelCursor_.column = {};
    if (unbox<unsigned int>(sixelCursor_.line) + sixelBandHeight_
        < unbox<unsigned int>(explicitSize_ ? size_.height : maxSize_.height))
        sixelCursor_.line = LineOffset::cast_from(sixelCursor_.line.as<unsigned int>() + sixelBandHeight_);
}

void SixelImageBuilder::setRaster(unsigned int _pan, unsigned int _pad, optional<ImageSize> _imageSize)
{
    if (_pad != 0)
        aspectRatio_ = max(
            1u, static_cast<unsigned int>(std::ceil(static_cast<float>(_pan) / static_cast<float>(_pad))));
    sixelBandHeight_ = 6 * aspectRatio_;
    if (_imageSize)
    {
        _imageSize->height = Height::cast_from(_imageSize->height.value * aspectRatio_);
        size_.width = clamp(_imageSize->width, Width(0), maxSize_.width);
        size_.height = clamp(_imageSize->height, Height(0), maxSize_.height);
        buffer_.resize(size_.area() * 4);
        explicitSize_ = true;
    }
}

void SixelImageBuilder::render(int8_t _sixel)
{
    // TODO: respect aspect ratio!
    auto const x = sixelCursor_.column;
    if (unbox<int>(x) < unbox<int>((explicitSize_ ? size_.width : maxSize_.width)))
    {
        for (unsigned int i = 0; i < 6; ++i)
        {
            auto const y = sixelCursor_.line + static_cast<int>(i * aspectRatio_);
            auto const pos = CellLocation { y, x };
            auto const pin = 1 << i;
            auto const pinned = (_sixel & pin) != 0;
            if (pinned)
                write(pos, currentColor());
        }
        sixelCursor_.column++;
    }
}

void SixelImageBuilder::finalize()
{
    if (unbox<int>(size_.height) == 1)
    {
        size_.height = Height::cast_from(sixelCursor_.line.as<unsigned int>() * aspectRatio_);
        buffer_.resize(size_.area() * 4);
        return;
    }
    if (!explicitSize_)
    {
        Buffer tempBuffer(static_cast<size_t>(size_.height.value * size_.width.value) * 4);
        for (auto i = 0; i < unbox<int>(size_.height); ++i)
        {
            for (auto j = 0; j < unbox<int>(size_.width); ++j)
            {
                std::copy_n(buffer_.begin() + i * unbox<long>(maxSize_.width) * 4,
                            size_.width.value * 4,
                            tempBuffer.begin() + i * unbox<long>(size_.width) * 4);
            }
        }
        buffer_.swap(tempBuffer);
        explicitSize_ = false;
    }
}

} // namespace terminal
