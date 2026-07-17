// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISColor.h>

#include <algorithm>
#include <cstdint>
#include <ranges>

namespace vtbackend::regis
{

namespace
{
    constexpr uint8_t percentToByte(unsigned pct) noexcept
    {
        return static_cast<uint8_t>((std::min(pct, 100u) * 255u) / 100u);
    }
} // namespace

RGBColor hlsToRgb(unsigned hueDeg, unsigned lightnessPct, unsigned saturationPct) noexcept
{
    // The DEC HLS conversion (and the VT340 default palette below) live in vtbackend/Color.h, shared
    // verbatim with the Sixel colour introducer so both graphics protocols render identical colours.
    return decHlsToRgb(hueDeg, lightnessPct, saturationPct);
}

RGBColor rgbPercentToRgb(unsigned redPct, unsigned greenPct, unsigned bluePct) noexcept
{
    return RGBColor { percentToByte(redPct), percentToByte(greenPct), percentToByte(bluePct) };
}

RGBColor grayToRgb(unsigned lightnessPct) noexcept
{
    auto const v = percentToByte(lightnessPct);
    return RGBColor { v, v, v };
}

void ReGISColorRegisters::reset() noexcept
{
    _registers = VT340DefaultColorPalette;
}

RGBColor ReGISColorRegisters::at(unsigned index) const noexcept
{
    return _registers[index % _registers.size()];
}

void ReGISColorRegisters::set(unsigned index, RGBColor color) noexcept
{
    _registers[index % _registers.size()] = color;
}

unsigned ReGISColorRegisters::findClosest(RGBColor color) const noexcept
{
    auto const squaredDistanceTo = [color](RGBColor c) noexcept {
        auto const dr = static_cast<int>(c.red) - static_cast<int>(color.red);
        auto const dg = static_cast<int>(c.green) - static_cast<int>(color.green);
        auto const db = static_cast<int>(c.blue) - static_cast<int>(color.blue);
        return (dr * dr) + (dg * dg) + (db * db);
    };
    auto const nearest = std::ranges::min_element(_registers, {}, squaredDistanceTo);
    return static_cast<unsigned>(std::ranges::distance(_registers.begin(), nearest));
}

} // namespace vtbackend::regis
