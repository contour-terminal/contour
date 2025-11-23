// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>

#include <crispy/overloaded.h>

#include <cstdio>
#include <map>

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
                    RGBColor { static_cast<uint8_t>(red ? ((red * 40) + 55) : 0),
                               static_cast<uint8_t>(green ? ((green * 40) + 55) : 0),
                               static_cast<uint8_t>(blue ? ((blue * 40) + 55) : 0) };

    // colors 232-255 are a grayscale ramp, intentionally leaving out black and white
    for (uint8_t gray = 0, level = uint8_t((gray * 10) + 8); gray < 24;
         ++gray, level = uint8_t((gray * 10) + 8))
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

bool defaultColorPalettes(std::string const& colorPaletteName, ColorPalette& palette) noexcept
{

    // TODO add dim colors, do we need to adapt them to each of color palettes?
    std::map<std::string, std::function<void()>> const definedColorPalettes = {
        { "contour", [&]() {} },
        { "monokai",
          [&]() {
              // Monokai dark colors
              // normal colors
              palette.palette[0] = 0x272822_rgb; // black
              palette.palette[1] = 0xf92672_rgb; // red
              palette.palette[2] = 0xa6e22e_rgb; // green
              palette.palette[3] = 0xf4bf75_rgb; // yellow
              palette.palette[4] = 0x66d9ef_rgb; // blue
              palette.palette[5] = 0xae81ff_rgb; // magenta
              palette.palette[6] = 0xa1efe4_rgb; // cyan
              palette.palette[7] = 0xf8f8f2_rgb; // white
              // bright colors
              palette.palette[8] = 0x75715e_rgb;  // bright black (dark gray)
              palette.palette[9] = 0xf92672_rgb;  // bright red
              palette.palette[10] = 0xa6e22e_rgb; // bright green
              palette.palette[11] = 0xf4bf75_rgb; // bright yellow
              palette.palette[12] = 0x66d9ef_rgb; // bright blue
              palette.palette[13] = 0xae81ff_rgb; // bright magenta
              palette.palette[14] = 0xa1efe4_rgb; // bright blue
              palette.palette[15] = 0xf8f8f2_rgb; // bright white

              palette.defaultForeground = 0xf8f8f2_rgb;
              palette.defaultBackground = 0x272822_rgb;
              palette.defaultForegroundBright = 0xf8f8f2_rgb;
              palette.defaultForegroundDimmed = 0x75715e_rgb;
              palette.mouseForeground = 0xf8f8f2_rgb;
              palette.mouseBackground = 0x272822_rgb;
              palette.cursor.color = 0xf8f8f2_rgb;
          } },
        { "one-light",
          [&]() {
              // One Light colors
              // normal colors
              palette.palette[0] = 0x000000_rgb; // black
              palette.palette[1] = 0xda3e39_rgb; // red
              palette.palette[2] = 0x41933e_rgb; // green
              palette.palette[3] = 0x855504_rgb; // yellow
              palette.palette[4] = 0x315eee_rgb; // blue
              palette.palette[5] = 0x930092_rgb; // magenta
              palette.palette[6] = 0x0e6fad_rgb; // cyan
              palette.palette[7] = 0x8e8f96_rgb; // white

              // bright colors
              palette.palette[8] = 0x2a2b32_rgb;  // bright black (dark gray)
              palette.palette[9] = 0xda3e39_rgb;  // bright red
              palette.palette[10] = 0x41933e_rgb; // bright green
              palette.palette[11] = 0x855504_rgb; // bright yellow
              palette.palette[12] = 0x315eee_rgb; // bright blue
              palette.palette[13] = 0x930092_rgb; // bright magenta
              palette.palette[14] = 0x0e6fad_rgb; // bright cuan
              palette.palette[15] = 0xfffefe_rgb; // bright white

              palette.defaultForeground = 0x2a2b32_rgb;
              palette.defaultBackground = 0xf8f8f8_rgb;
              palette.cursor.color = 0x2a2b32_rgb;
          } },
        { "one-dark",
          [&]() {
              // One Dark colors
              // normal colors
              palette.palette[0] = 0x000000_rgb; // black
              palette.palette[1] = 0xe06c75_rgb; // red
              palette.palette[2] = 0x98c379_rgb; // green
              palette.palette[3] = 0xe5c07b_rgb; // yellow
              palette.palette[4] = 0x61afef_rgb; // blue
              palette.palette[5] = 0xc678dd_rgb; // magenta
              palette.palette[6] = 0x56b6c2_rgb; // cyan
              palette.palette[7] = 0xabb2bf_rgb; // white

              // bright colors
              palette.palette[8] = 0x5c6370_rgb;  // bright black (dark gray)
              palette.palette[9] = 0xe06c75_rgb;  // bright red
              palette.palette[10] = 0x98c379_rgb; // bright green
              palette.palette[11] = 0xd19a66_rgb; // bright yellow
              palette.palette[12] = 0x61afef_rgb; // bright blue
              palette.palette[13] = 0xc678dd_rgb; // bright magenta
              palette.palette[14] = 0x56b6c2_rgb; // bright cuan
              palette.palette[15] = 0xfffefe_rgb; // bright white

              palette.defaultForeground = 0x5c6370_rgb;
              palette.defaultBackground = 0x1e2127_rgb;
              palette.defaultForegroundBright = 0x5c6370_rgb;
              palette.defaultForegroundDimmed = 0x545862_rgb;
              palette.mouseForeground = 0xabb2bf_rgb;
              palette.mouseBackground = 0x282c34_rgb;
              palette.cursor.color = 0x5c6370_rgb;
          } },
        { "gruvbox-light",
          [&]() {
              // Gruvbox colors
              // normal colors
              palette.palette[0] = 0xfbf1c7_rgb; // black
              palette.palette[1] = 0xcc241d_rgb; // red
              palette.palette[2] = 0x98971a_rgb; // green
              palette.palette[3] = 0xd79921_rgb; // yellow
              palette.palette[4] = 0x458588_rgb; // blue
              palette.palette[5] = 0xb16286_rgb; // magenta
              palette.palette[6] = 0x689d6a_rgb; // cyan
              palette.palette[7] = 0x7c6f64_rgb; // white
              // bright colors
              palette.palette[8] = 0x928374_rgb;  // bright black (dark gray)
              palette.palette[9] = 0x9d0006_rgb;  // bright red
              palette.palette[10] = 0x79740e_rgb; // bright green
              palette.palette[11] = 0xb57614_rgb; // bright yellow
              palette.palette[12] = 0x076678_rgb; // bright blue
              palette.palette[13] = 0x8f3f71_rgb; // bright magenta
              palette.palette[14] = 0x427b58_rgb; // bright cuan
              palette.palette[15] = 0x3c3836_rgb; // bright white

              palette.defaultForeground = 0x3c3836_rgb;
              palette.defaultBackground = 0xfbf1c7_rgb;
              palette.cursor.color = 0x3c3836_rgb;
          } },
        { "gruvbox-dark",
          [&]() {
              // Gruvbox colors
              // normal colors
              palette.palette[0] = 0x282828_rgb; // black
              palette.palette[1] = 0xcc241d_rgb; // red
              palette.palette[2] = 0x98971a_rgb; // green
              palette.palette[3] = 0xd79921_rgb; // yellow
              palette.palette[4] = 0x458588_rgb; // blue
              palette.palette[5] = 0xb16286_rgb; // magenta
              palette.palette[6] = 0x689d6a_rgb; // cyan
              palette.palette[7] = 0xa89984_rgb; // white
              // bright colors
              palette.palette[8] = 0x928374_rgb;  // bright black (dark gray)
              palette.palette[9] = 0xfb4934_rgb;  // bright red
              palette.palette[10] = 0xb8bb26_rgb; // bright green
              palette.palette[11] = 0xfabd2f_rgb; // bright yellow
              palette.palette[12] = 0x83a598_rgb; // bright blue
              palette.palette[13] = 0xd3869b_rgb; // bright magenta
              palette.palette[14] = 0x8ec07c_rgb; // bright cuan
              palette.palette[15] = 0xebdbb2_rgb; // bright white

              palette.defaultForeground = 0xebdbb2_rgb;
              palette.defaultBackground = 0x292929_rgb;
              palette.cursor.color = 0xebdbb2_rgb;
          } },
        { "solarized-light",
          [&]() {
              // Solarized colors
              // normal colors
              palette.palette[0] = 0xeee8d5_rgb; // black
              palette.palette[1] = 0xdc322f_rgb; // red
              palette.palette[2] = 0x859900_rgb; // green
              palette.palette[3] = 0xb58900_rgb; // yellow
              palette.palette[4] = 0x268bd2_rgb; // blue
              palette.palette[5] = 0xd33682_rgb; // magenta
              palette.palette[6] = 0x2aa198_rgb; // cyan
              palette.palette[7] = 0x002b36_rgb; // white
              // bright colors
              palette.palette[8] = 0x657b83_rgb;  // bright black
              palette.palette[9] = 0xcb4b16_rgb;  // bright red
              palette.palette[10] = 0x859900_rgb; // bright green
              palette.palette[11] = 0xb58900_rgb; // bright yellow
              palette.palette[12] = 0x6c71c4_rgb; // bright blue
              palette.palette[13] = 0xd33682_rgb; // bright magenta
              palette.palette[14] = 0x2aa198_rgb; // bright cyan
              palette.palette[15] = 0x073642_rgb; // bright white

              palette.defaultForeground = 0x657b83_rgb;
              palette.defaultBackground = 0xfdf6e3_rgb;
              palette.cursor.color = 0x657b83_rgb;
          } },
        { "solarized-dark",
          [&]() {
              // solarized colors
              // normal colors
              palette.palette[0] = 0x073642_rgb; // black
              palette.palette[1] = 0xdc322f_rgb; // red
              palette.palette[2] = 0x859900_rgb; // green
              palette.palette[3] = 0xcf9a6b_rgb; // yellow
              palette.palette[4] = 0x268bd2_rgb; // blue
              palette.palette[5] = 0xd33682_rgb; // magenta
              palette.palette[6] = 0x2aa198_rgb; // cyan
              palette.palette[7] = 0xeee8d5_rgb; // white
              // bright color
              palette.palette[8] = 0x657b83_rgb;  // bright black
              palette.palette[9] = 0xcb4b16_rgb;  // bright red
              palette.palette[10] = 0x859900_rgb; // bright green
              palette.palette[11] = 0xcf9a6b_rgb; // bright yellow
              palette.palette[12] = 0x6c71c4_rgb; // bright blue
              palette.palette[13] = 0xd33682_rgb; // bright magenta
              palette.palette[14] = 0x2aa198_rgb; // bright cyan
              palette.palette[15] = 0xfdf6e3_rgb; // bright white

              palette.defaultForeground = 0x839496_rgb;
              palette.defaultBackground = 0x002b36_rgb;
              palette.cursor.color = 0x839496_rgb;
          } },
        { "papercolor-light",
          [&]() {
              // papercolor light colors
              // normal colors
              palette.palette[0] = 0xeeeeee_rgb; // black
              palette.palette[1] = 0xaf0000_rgb; // red
              palette.palette[2] = 0x008700_rgb; // green
              palette.palette[3] = 0x5f8700_rgb; // yellow
              palette.palette[4] = 0x0087af_rgb; // blue
              palette.palette[5] = 0x878787_rgb; // magenta
              palette.palette[6] = 0x005f87_rgb; // cyan
              palette.palette[7] = 0x444444_rgb; // white
              // bright colors
              palette.palette[8] = 0xbcbcbc_rgb;  // bright black
              palette.palette[9] = 0xd70000_rgb;  // bright red
              palette.palette[10] = 0xd70087_rgb; // bright green
              palette.palette[11] = 0x8700af_rgb; // bright yellow
              palette.palette[12] = 0xd75f00_rgb; // bright blue
              palette.palette[13] = 0xd75f00_rgb; // bright magenta
              palette.palette[14] = 0x005faf_rgb; // bright cyan
              palette.palette[15] = 0x005f87_rgb; // bright white

              palette.defaultForeground = 0x444444_rgb;
              palette.defaultBackground = 0xeeeeee_rgb;
              palette.cursor.color = 0x444444_rgb;
          } },
        { "papercolor-dark",
          [&]() {
              // papercolor dark colors
              // normal colors
              palette.palette[0] = 0x1C1C1C_rgb; // Black (Host)
              palette.palette[1] = 0xAF005F_rgb; // Red (Syntax string)
              palette.palette[2] = 0x5FAF00_rgb; // Green (Command)
              palette.palette[3] = 0xD7AF5F_rgb; // Yellow (Command second)
              palette.palette[4] = 0x5FAFD7_rgb; // Blue (Path)
              palette.palette[5] = 0x808080_rgb; // Magenta (Syntax var)
              palette.palette[6] = 0xD7875F_rgb; // Cyan (Prompt)
              palette.palette[7] = 0xD0D0D0_rgb; // White

              palette.palette[8] = 0x585858_rgb;  // Bright Black
              palette.palette[9] = 0x5FAF5F_rgb;  // Bright Red (Command error)
              palette.palette[10] = 0xAFD700_rgb; // Bright Green (Exec)
              palette.palette[11] = 0xAF87D7_rgb; // Bright Yellow
              palette.palette[12] = 0xFFAF00_rgb; // Bright Blue (Folder)
              palette.palette[13] = 0xFF5FAF_rgb; // Bright Magenta
              palette.palette[14] = 0x00AFAF_rgb; // Bright Cyan
              palette.palette[15] = 0x5F8787_rgb; // Bright White

              palette.defaultForeground = 0xd0d0d0_rgb;
              palette.defaultBackground = 0x1c1c1c_rgb;
              palette.cursor.color = 0xd0d0d0_rgb;
          } },
    };
    // find if colorPaletteName is a known color palette
    auto const it = definedColorPalettes.find(colorPaletteName);
    if (it != definedColorPalettes.end())
    {
        it->second();
        return true;
    }
    return false;
}

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
