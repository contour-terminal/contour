// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/Image.h>

#include <crispy/StrongHash.h>

#include <fmt/format.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <variant>

namespace vtbackend
{

enum class ColorPreference
{
    Dark,
    Light,
};

struct ImageData
{
    vtbackend::ImageFormat format;
    int rowAlignment = 1;
    ImageSize size;
    std::vector<uint8_t> pixels;

    crispy::strong_hash hash;

    void updateHash() noexcept;
};

using ImageDataPtr = std::shared_ptr<ImageData const>;

struct BackgroundImage
{
    using Location = std::variant<std::filesystem::path, ImageDataPtr>;

    Location location;
    crispy::strong_hash hash;

    // image configuration
    float opacity = 0.5; // normalized value
    bool blur = false;
};

struct ColorPalette
{
    using Palette = std::array<RGBColor, 256 + 8>;

    /// Indicates whether or not bright colors are being allowed
    /// for indexed colors between 0..7 and mode set to ColorMode::Bright.
    ///
    /// This value is used by draw_bold_text_with_bright_colors in profile configuration.
    ///
    /// If disabled, normal color will be used instead.
    ///
    /// TODO: This should be part of Config's Profile instead of being here. That sounds just wrong.
    /// TODO: And even the naming sounds wrong. Better would be makeIndexedColorsBrightForBoldText or similar.
    bool useBrightColors = false;

    static Palette const defaultColorPalette;

    Palette palette = defaultColorPalette;

    [[nodiscard]] RGBColor normalColor(size_t index) const noexcept
    {
        assert(index < 8);
        return palette.at(index);
    }

    [[nodiscard]] RGBColor brightColor(size_t index) const noexcept
    {
        assert(index < 8);
        return palette.at(index + 8);
    }

    [[nodiscard]] RGBColor dimColor(size_t index) const noexcept
    {
        assert(index < 8);
        return palette[256 + index];
    }

    [[nodiscard]] RGBColor indexedColor(size_t index) const noexcept
    {
        assert(index < 256);
        return palette.at(index);
    }

    RGBColor defaultForeground = 0xD0D0D0_rgb;
    RGBColor defaultBackground = 0x1a1716_rgb;
    RGBColor defaultForegroundBright = 0xFFFFFF_rgb;
    RGBColor defaultForegroundDimmed = 0x808080_rgb;

    CursorColor cursor;

    RGBColor mouseForeground = 0x800000_rgb;
    RGBColor mouseBackground = 0x808000_rgb;

    struct
    {
        RGBColor normal = 0xF0F000_rgb;
        RGBColor hover = 0xFF0000_rgb;
    } hyperlinkDecoration;

    RGBColorPair inputMethodEditor = { 0xFFFFFF_rgb, 0xFF0000_rgb };

    std::shared_ptr<BackgroundImage> backgroundImage;

    // clang-format off
    CellRGBColorAndAlphaPair yankHighlight { CellForegroundColor {}, 1.0f, 0xffA500_rgb, 0.5f };

    CellRGBColorAndAlphaPair searchHighlight { CellBackgroundColor {}, 1.0f, CellForegroundColor {}, 1.0f };
    CellRGBColorAndAlphaPair searchHighlightFocused {  CellBackgroundColor {}, 1.0f,CellForegroundColor {}, 1.0f };

    CellRGBColorAndAlphaPair wordHighlight { CellForegroundColor {}, 1.0f, 0x909090_rgb, 0.5f };
    CellRGBColorAndAlphaPair wordHighlightCurrent { CellForegroundColor {}, 1.0f, RGBColor{0x90, 0x90, 0x90}, 0.6f };

    CellRGBColorAndAlphaPair selection { CellForegroundColor {}, 1.0f, 0x4040f0_rgb , 0.5f };

    CellRGBColorAndAlphaPair normalModeCursorline = { 0xFFFFFF_rgb, 0.2f, 0x808080_rgb, 0.4f };
    // clang-format on

    RGBColorPair indicatorStatusLine = { 0x808080_rgb, 0x000000_rgb };
    RGBColorPair indicatorStatusLineInactive = { 0x808080_rgb, 0x000000_rgb };
};

enum class ColorTarget
{
    Foreground,
    Background,
};

enum class ColorMode
{
    Dimmed,
    Normal,
    Bright
};

RGBColor apply(ColorPalette const& colorPalette, Color color, ColorTarget target, ColorMode mode) noexcept;

} // namespace vtbackend

// {{{ fmtlib custom formatter support
template <>
struct fmt::formatter<vtbackend::ColorPreference>: fmt::formatter<std::string_view>
{
    auto format(vtbackend::ColorPreference value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ColorPreference::Dark: name = "Dark"; break;
            case vtbackend::ColorPreference::Light: name = "Light"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::ColorMode>: fmt::formatter<std::string_view>
{
    auto format(vtbackend::ColorMode value, fmt::format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ColorMode::Normal: name = "Normal"; break;
            case vtbackend::ColorMode::Dimmed: name = "Dimmed"; break;
            case vtbackend::ColorMode::Bright: name = "Bright"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::ColorTarget>: fmt::formatter<std::string_view>
{
    auto format(vtbackend::ColorTarget value, fmt::format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ColorTarget::Foreground: name = "Foreground"; break;
            case vtbackend::ColorTarget::Background: name = "Background"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
// }}}
