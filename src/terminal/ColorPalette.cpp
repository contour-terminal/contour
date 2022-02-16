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
#include <terminal/Color.h>
#include <terminal/ColorPalette.h>

#include <crispy/overloaded.h>

#include <cstdio>

using namespace std;

namespace terminal
{

namespace
{
    template <typename T>
    constexpr T roundUp(T numToRound, T multiple) noexcept
    {
        if (multiple == 0)
            return numToRound;

        T remainder = numToRound % multiple;
        if (remainder == 0)
            return numToRound;

        return numToRound + multiple - remainder;
    }
} // namespace

void ImageData::updateHash() noexcept
{
    // clang-format off
    using crispy::StrongHash;
    auto hashValue = StrongHash(0, 0, 0, size.width.value)
                   * static_cast<uint32_t>(size.height.value)
                   * static_cast<uint32_t>(rowAlignment)
                   * static_cast<uint32_t>(format);
    uint8_t const* scanLine = pixels.data();
    auto const scanLineLength = unbox<size_t>(size.width);
    auto const pitch = roundUp(scanLineLength, static_cast<size_t>(rowAlignment));
    for (unsigned row = 0; row < size.height.value; ++row)
    {
        hashValue = hashValue * StrongHash::compute(scanLine, scanLineLength);
        scanLine += pitch;
    }
    hash = hashValue;
    // clang-format off
}

RGBColor apply(ColorPalette const& _profile, Color _color, ColorTarget _target, bool _bright) noexcept
{
    switch (_color.type())
    {
    case ColorType::RGB: return _color.rgb();
    case ColorType::Indexed: {
        auto const index = static_cast<size_t>(_color.index());
        if (_bright && index < 8)
            return _profile.brightColor(index);
        else
            return _profile.indexedColor(index);
        break;
    }
    case ColorType::Bright: return _profile.brightColor(static_cast<size_t>(_color.index()));
    case ColorType::Undefined:
    case ColorType::Default: break;
    }
    return _target == ColorTarget::Foreground ? _profile.defaultForeground : _profile.defaultBackground;
}

} // namespace terminal
