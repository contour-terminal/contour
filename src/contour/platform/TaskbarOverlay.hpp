// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <string>

namespace contour::platform
{

/// The largest count the overlay icon spells out; more shows as "9+" since the icon is 16 pixels wide.
inline constexpr unsigned OverlayMaxDigit = 9;

/// The fill of the overlay's disc: amber, the hue the tab strip's waiting badge wears. The taskbar's own
/// colour is not ours to read, so this is a fixed, bundled-scheme-independent warning colour.
inline constexpr auto OverlayDiscColor = vtbackend::RGBColor { 0xF5, 0xA6, 0x23 };

/// The 1 px ring around the disc. Amber alone is about 1.9:1 on the light Windows 11 taskbar, short of the
/// 3:1 a mark needs; this dark brown holds it there, while the disc itself carries the dark taskbar.
inline constexpr auto OverlayOutlineColor = vtbackend::RGBColor { 0x4A, 0x2C, 0x00 };

/// The ring's width in pixels at the icon's logical size; the painter scales it with the pixel ratio.
inline constexpr unsigned OverlayOutlineWidth = 1;

/// The taskbar colours the overlay must stay visible on: the Windows 11 light and dark taskbars. The
/// taskbar's actual colour is not ours to read, so these bracket it.
inline constexpr auto OverlayTaskbarBackgrounds = std::array {
    vtbackend::RGBColor { 0xF3, 0xF3, 0xF3 }, // light
    vtbackend::RGBColor { 0x20, 0x20, 0x20 }, // dark
};

/// How large the count is drawn, for a text of a given length: the icon is 16 logical pixels wide, so "9+"
/// has to be set smaller than a single digit to fit inside the disc.
struct OverlayTextSize
{
    size_t characters; ///< The text length this row is for.
    unsigned eighths;  ///< The glyph pixel size, in eighths of the icon's pixel size.
};

/// One row per text length the overlay spells; the last row also serves any longer text.
inline constexpr auto OverlayTextSizes = std::array {
    OverlayTextSize { .characters = 1, .eighths = 6 },
    OverlayTextSize { .characters = 2, .eighths = 4 },
};

/// @param iconSize The icon's edge in device pixels.
/// @param characters The length of the text drawn in it.
/// @return The glyph pixel size that makes the text fit the disc.
[[nodiscard]] constexpr int overlayTextPixelSize(int iconSize, size_t characters) noexcept
{
    auto eighths = OverlayTextSizes.back().eighths;
    for (auto const& row: OverlayTextSizes)
        if (characters <= row.characters)
        {
            eighths = row.eighths;
            break;
        }
    return iconSize * static_cast<int>(eighths) / 8;
}

/// @return Black or white, whichever reads better on @p disc.
[[nodiscard]] inline vtbackend::RGBColor overlayInkFor(vtbackend::RGBColor disc) noexcept
{
    constexpr auto Black = vtbackend::RGBColor { 0, 0, 0 };
    constexpr auto White = vtbackend::RGBColor { 0xFF, 0xFF, 0xFF };
    return vtbackend::contrastRatio(Black, disc) >= vtbackend::contrastRatio(White, disc) ? Black : White;
}

/// The text drawn in the overlay icon's disc.
/// @param waiting How many tabs wait for the user.
/// @return Nothing for none (no overlay), the number up to 9, then "9+".
[[nodiscard]] inline std::string overlayBadgeText(unsigned waiting)
{
    if (waiting == 0)
        return {};
    return waiting > OverlayMaxDigit ? std::format("{}+", OverlayMaxDigit) : std::to_string(waiting);
}

/// The overlay's accessible description, read by screen readers.
/// @param waiting How many tabs wait for the user.
/// @return Nothing for none, else "1 tab waiting for you" or "<n> tabs waiting for you".
[[nodiscard]] inline std::string overlayDescription(unsigned waiting)
{
    if (waiting == 0)
        return {};
    return waiting == 1 ? std::string { "1 tab waiting for you" }
                        : std::format("{} tabs waiting for you", waiting);
}

} // namespace contour::platform
