// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>

#include <crispy/overloaded.h>

#include <cstdio>

using namespace std;

namespace vtbackend
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

ColorPalette::Palette const ColorPalette::defaultColorPalette = []() constexpr {
    ColorPalette::Palette colors;

    // normal colors
    colors[0] = 0x000000_rgb; // black
    colors[1] = 0xc63939_rgb; // red
    colors[2] = 0x00a000_rgb; // green
    colors[3] = 0xa0a000_rgb; // yellow
    colors[4] = 0x4d79ff_rgb; // blue
    colors[5] = 0xff66ff_rgb; // magenta
    colors[6] = 0x00a0a0_rgb; // cyan
    colors[7] = 0xc0c0c0_rgb; // white

    // bright colors
    colors[8] = 0x707070_rgb;  // bright black (dark gray)
    colors[9] = 0xff0000_rgb;  // bright red
    colors[10] = 0x00ff00_rgb; // bright green
    colors[11] = 0xffff00_rgb; // bright yellow
    colors[12] = 0x0000ff_rgb; // bright blue
    colors[13] = 0xff00ff_rgb; // bright magenta
    colors[14] = 0x00ffff_rgb; // bright blue
    colors[15] = 0xffffff_rgb; // bright white

    // colors 16-231 are a 6x6x6 color cube
    for (unsigned red = 0; red < 6; ++red)
        for (unsigned green = 0; green < 6; ++green)
            for (unsigned blue = 0; blue < 6; ++blue)
                colors[16 + (red * 36) + (green * 6) + blue] =
                    RGBColor { static_cast<uint8_t>(red ? (red * 40 + 55) : 0),
                               static_cast<uint8_t>(green ? (green * 40 + 55) : 0),
                               static_cast<uint8_t>(blue ? (blue * 40 + 55) : 0) };

    // colors 232-255 are a grayscale ramp, intentionally leaving out black and white
    for (uint8_t gray = 0, level = uint8_t(gray * 10 + 8); gray < 24; ++gray, level = uint8_t(gray * 10 + 8))
        colors[size_t(232 + gray)] = RGBColor { level, level, level };

    // dim colors
    colors[256 + 0] = 0x000000_rgb; // black
    colors[256 + 1] = 0xa00000_rgb; // red
    colors[256 + 2] = 0x008000_rgb; // green
    colors[256 + 3] = 0x808000_rgb; // yellow
    colors[256 + 4] = 0x000080_rgb; // blue
    colors[256 + 5] = 0x800080_rgb; // magenta
    colors[256 + 6] = 0x008080_rgb; // cyan
    colors[256 + 7] = 0x808080_rgb; // white

    return colors;
}();

void ImageData::updateHash() noexcept
{
    // clang-format off
    auto hashValue = crispy::strong_hash(0, 0, 0, size.width.value)
                   * static_cast<uint32_t>(size.height.value)
                   * static_cast<uint32_t>(rowAlignment)
                   * static_cast<uint32_t>(format);
    uint8_t const* scanLine = pixels.data();
    auto const scanLineLength = unbox<size_t>(size.width);
    auto const pitch = roundUp(scanLineLength, static_cast<size_t>(rowAlignment));
    for (unsigned row = 0; row < size.height.value; ++row)
    {
        hashValue = hashValue * crispy::strong_hash::compute(scanLine, scanLineLength);
        scanLine += pitch;
    }
    hash = hashValue;
    // clang-format on
}

RGBColor apply(ColorPalette const& colorPalette, Color color, ColorTarget target, ColorMode mode) noexcept
{
    switch (color.type())
    {
        case ColorType::RGB: {
            return color.rgb();
        }
        case ColorType::Indexed: {
            auto const index = static_cast<size_t>(color.index());
            if (mode == ColorMode::Bright && index < 8)
                return colorPalette.brightColor(index);
            else if (mode == ColorMode::Dimmed && index < 8)
                return colorPalette.dimColor(index);
            else
                return colorPalette.indexedColor(index);
        }
        case ColorType::Bright: {
            return colorPalette.brightColor(static_cast<size_t>(color.index()));
        }
        case ColorType::Undefined:
        case ColorType::Default: {
            if (target == ColorTarget::Foreground)
            {
                switch (mode)
                {
                    case ColorMode::Normal: return colorPalette.defaultForeground;
                    case ColorMode::Bright: return colorPalette.defaultForegroundBright;
                    case ColorMode::Dimmed: return colorPalette.defaultForegroundDimmed;
                }
            }
            else
            {
                return colorPalette.defaultBackground;
            }
        }
    }
    crispy::unreachable();
}

} // namespace vtbackend
