// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace contour::config
{

/// Which edge the scrollbar floats against (`scrollbar.position`), or none.
///
/// Kept here rather than in Config.hpp so that the pure scrollbar decisions (session/ScrollbarVisibility.hpp)
/// can name it without pulling in the whole configuration model.
enum class ScrollBarPosition : uint8_t
{
    Hidden, //!< No scrollbar, and with it no marks.
    Left,   //!< Against the left edge of the pane.
    Right   //!< Against the right edge of the pane.
};

/// How the scrollbar behaves at the bottom of the scrollback (spec §3.11).
///
/// Zero is `Overlay`, so a zero-initialised value is the default the spec names.
enum class ScrollBarMode : uint8_t
{
    Overlay = 0, //!< Hidden at the bottom of the scrollback unless a held mark is above the viewport.
    Always,      //!< Shown whenever there is scrollback.
};

/// What `scrollbar.mode` asks for: one of the two modes, or whatever the operating system uses.
///
/// Zero is `System`: the bar follows the system's scroller style unless the profile says otherwise.
enum class ScrollBarModePreference : uint8_t
{
    System = 0, //!< Follow the operating system's scroller style (@see resolveScrollBarMode).
    Overlay,    //!< Always ScrollBarMode::Overlay.
    Always,     //!< Always ScrollBarMode::Always.
};

namespace detail
{
    /// The `scrollbar.mode` spellings. Written into every generated `contour.yml`, so fixed once shipped.
    inline constexpr auto ScrollBarModePreferenceTable = std::array {
        ConfigEnumInfo<ScrollBarModePreference> {
            ScrollBarModePreference::System, "system", "Follow the system" },
        ConfigEnumInfo<ScrollBarModePreference> { ScrollBarModePreference::Overlay, "overlay", "Overlay" },
        ConfigEnumInfo<ScrollBarModePreference> { ScrollBarModePreference::Always, "always", "Always" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<ScrollBarModePreference> const> configEnumValues() noexcept
{
    return detail::ScrollBarModePreferenceTable;
}

/// The mode the scrollbar uses, given what the profile asks for and what the system publishes.
/// @param preference The profile's `scrollbar.mode`.
/// @param system The system's scroller style, or nullopt where the platform publishes none.
/// @return @p system (Overlay when it is absent) for ScrollBarModePreference::System; otherwise the mode
///         @p preference names, whatever the system says.
[[nodiscard]] constexpr ScrollBarMode resolveScrollBarMode(ScrollBarModePreference preference,
                                                           std::optional<ScrollBarMode> system) noexcept
{
    switch (preference)
    {
        case ScrollBarModePreference::System: return system.value_or(ScrollBarMode::Overlay);
        case ScrollBarModePreference::Overlay: return ScrollBarMode::Overlay;
        case ScrollBarModePreference::Always: return ScrollBarMode::Always;
    }
    return ScrollBarMode::Overlay;
}

} // namespace contour::config
