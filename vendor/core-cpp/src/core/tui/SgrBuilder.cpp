// SPDX-License-Identifier: Apache-2.0
#include <core/tui/SgrBuilder.hpp>

#include <core/tui/TerminalProtocols.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <variant>

namespace core::tui
{

std::string buildSgrReset()
{
    return std::string { protocols::SgrReset };
}

std::string buildSgrSequence(Style const& style)
{
    // Compute effective underline style
    auto const underlineFromFlag = style.underline ? UnderlineStyle::Single : UnderlineStyle::None;
    auto const effectiveUnderline =
        style.underlineStyle != UnderlineStyle::None ? style.underlineStyle : underlineFromFlag;
    auto const isDefaultUlColor = std::holds_alternative<std::monostate>(style.underlineColor);

    // Check if style is default (no attributes set)
    auto const isDefaultFg = std::holds_alternative<std::monostate>(style.fg);
    auto const isDefaultBg = std::holds_alternative<std::monostate>(style.bg);
    if (isDefaultFg && isDefaultBg && !style.bold && !style.italic && !style.strikethrough && !style.dim
        && !style.inverse && effectiveUnderline == UnderlineStyle::None)
        return {};

    std::string result = "\033[";
    auto needSemicolon = false;
    auto const appendSep = [&]() {
        if (needSemicolon)
            result += ';';
        needSemicolon = true;
    };

    if (style.bold)
    {
        appendSep();
        result += '1';
    }
    if (style.dim)
    {
        appendSep();
        result += '2';
    }
    if (style.italic)
    {
        appendSep();
        result += '3';
    }
    if (effectiveUnderline != UnderlineStyle::None)
    {
        appendSep();
        result += std::format("4:{}", static_cast<int>(effectiveUnderline));
    }
    if (style.inverse)
    {
        appendSep();
        result += '7';
    }
    if (style.strikethrough)
    {
        appendSep();
        result += '9';
    }

    // Foreground color
    if (auto const* idx = std::get_if<std::uint8_t>(&style.fg))
    {
        appendSep();
        result += std::format("38;5;{}", *idx);
    }
    else if (auto const* rgb = std::get_if<RgbColor>(&style.fg))
    {
        appendSep();
        result += std::format("38;2;{};{};{}", rgb->r, rgb->g, rgb->b);
    }

    // Background color
    if (auto const* idx = std::get_if<std::uint8_t>(&style.bg))
    {
        appendSep();
        result += std::format("48;5;{}", *idx);
    }
    else if (auto const* rgb = std::get_if<RgbColor>(&style.bg))
    {
        appendSep();
        result += std::format("48;2;{};{};{}", rgb->r, rgb->g, rgb->b);
    }

    result += 'm';

    // Underline color (SGR 58) — emitted as a separate sequence to avoid exceeding
    // the 16-element parameter array limit in some terminals (e.g. contour) when
    // combined with fg/bg colors in the same sequence.
    if (!isDefaultUlColor)
    {
        if (auto const* idx = std::get_if<std::uint8_t>(&style.underlineColor))
            result += std::format("\033[58:5:{}m", *idx);
        else if (auto const* rgb = std::get_if<RgbColor>(&style.underlineColor))
            result += std::format("\033[58:2:{}:{}:{}m", rgb->r, rgb->g, rgb->b);
    }

    return result;
}

} // namespace core::tui
