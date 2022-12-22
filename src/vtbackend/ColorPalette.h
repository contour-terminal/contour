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
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/Image.h>

#include <crispy/StrongHash.h>
#include <crispy/stdfs.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>

namespace terminal
{

struct ImageData
{
    terminal::ImageFormat format;
    int rowAlignment = 1;
    ImageSize size;
    std::vector<uint8_t> pixels;

    crispy::StrongHash hash;

    void updateHash() noexcept;
};

using ImageDataPtr = std::shared_ptr<ImageData const>;

struct BackgroundImage
{
    using Location = std::variant<FileSystem::path, ImageDataPtr>;

    Location location;
    crispy::StrongHash hash;

    // image configuration
    float opacity = 1.0; // normalized value
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

    [[nodiscard]] RGBColor normalColor(size_t _index) const noexcept
    {
        assert(_index < 8);
        return palette.at(_index);
    }

    [[nodiscard]] RGBColor brightColor(size_t _index) const noexcept
    {
        assert(_index < 8);
        return palette.at(_index + 8);
    }

    [[nodiscard]] RGBColor dimColor(size_t _index) const noexcept
    {
        assert(_index < 8);
        return palette[256 + _index];
    }

    [[nodiscard]] RGBColor indexedColor(size_t _index) const noexcept
    {
        assert(_index < 256);
        return palette.at(_index);
    }

    RGBColor defaultForeground = 0xD0D0D0_rgb;
    RGBColor defaultBackground = 0x000000_rgb;

    CursorColor cursor;

    RGBColor mouseForeground = 0x800000_rgb;
    RGBColor mouseBackground = 0x808000_rgb;

    struct
    {
        RGBColor normal = 0x0070F0_rgb;
        RGBColor hover = 0xFF0000_rgb;
    } hyperlinkDecoration;

    std::shared_ptr<BackgroundImage const> backgroundImage;

    // clang-format off
    CellRGBColorAndAlphaPair yankHighlight { CellForegroundColor {}, 1.0f, 0xffA500_rgb, 0.5f };
    CellRGBColorAndAlphaPair searchHighlight { CellBackgroundColor {}, 1.0f, CellForegroundColor {}, 1.0f };
    CellRGBColorAndAlphaPair searchHighlightFocused { CellForegroundColor {}, 1.0f, RGBColor{0xFF, 0x30, 0x30}, 0.5f };
    CellRGBColorAndAlphaPair selection { CellBackgroundColor {}, 1.0f, CellForegroundColor {}, 1.0f };
    // clang-format on

    RGBColorPair indicatorStatusLine = { 0x000000_rgb, 0x808080_rgb };
    RGBColorPair indicatorStatusLineInactive = { 0x000000_rgb, 0x808080_rgb };
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

RGBColor apply(ColorPalette const& profile, Color color, ColorTarget target, ColorMode mode) noexcept;

} // namespace terminal

namespace fmt // {{{
{
template <>
struct formatter<terminal::ColorMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::ColorMode value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::ColorMode::Normal: return fmt::format_to(ctx.out(), "Normal");
            case terminal::ColorMode::Dimmed: return fmt::format_to(ctx.out(), "Dimmed");
            case terminal::ColorMode::Bright: return fmt::format_to(ctx.out(), "Bright");
        }
        return fmt::format_to(ctx.out(), "{}", (int) value);
    }
};

template <>
struct formatter<terminal::ColorTarget>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::ColorTarget value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::ColorTarget::Foreground: return fmt::format_to(ctx.out(), "Foreground");
            case terminal::ColorTarget::Background: return fmt::format_to(ctx.out(), "Background");
        }
        return fmt::format_to(ctx.out(), "{}", (int) value);
    }
};
} // namespace fmt
// }}}
