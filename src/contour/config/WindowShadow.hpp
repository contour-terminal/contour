// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnum.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace contour::config
{

/// How far the window's drop shadow reaches.
///
/// The enumerators, their spellings and their sizes are Breeze's, deliberately: these are the names
/// a Plasma user already chose from in System Settings -> Appearance -> Window Decorations, and a
/// shadow that does not match the one on the window next to it reads as a bug rather than a style.
/// Breeze offers exactly these five; there is no larger size to add.
///
/// @note Only KWin can act on this. No other compositor offers a way for an undecorated window to
///       publish a shadow, so on GNOME, wlroots and friends every value behaves as @c None.
enum class ShadowSize : uint8_t
{
    None = 0,  //!< No shadow at all.
    Small,     //!< A tight shadow that barely leaves the window edge.
    Medium,    //!< Half of Large.
    Large,     //!< Breeze's own default, and ours.
    VeryLarge, //!< The most reach Breeze offers.
};

namespace detail
{
    // inline, so every translation unit that includes this header shares one table rather than
    // getting a private copy the returned span would then point into.
    inline constexpr auto ShadowSizeTable = std::array {
        ConfigEnumInfo<ShadowSize> { ShadowSize::None, "None", "No Shadow" },
        ConfigEnumInfo<ShadowSize> { ShadowSize::Small, "Small", "Small" },
        ConfigEnumInfo<ShadowSize> { ShadowSize::Medium, "Medium", "Medium" },
        ConfigEnumInfo<ShadowSize> { ShadowSize::Large, "Large", "Large" },
        ConfigEnumInfo<ShadowSize> { ShadowSize::VeryLarge, "VeryLarge", "Very Large" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<ShadowSize> const> configEnumValues() noexcept
{
    return detail::ShadowSizeTable;
}

} // namespace contour::config
