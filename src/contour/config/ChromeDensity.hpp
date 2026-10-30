// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace contour::config
{

/// How much room the window chrome takes.
///
/// Zero is `Comfortable`, the default in both defaults editions.
enum class ChromeDensity : uint8_t
{
    Comfortable = 0, //!< Pane cards with headers and a gap, once a tab is split.
    Minimal,         //!< Bare split lines; headers only while the modifier is held.
};

/// The number of ChromeDensity enumerators, derived from the last one.
inline constexpr size_t ChromeDensityCount = static_cast<size_t>(ChromeDensity::Minimal) + 1;

namespace detail
{
    /// The density spellings. The tokens are written into the configuration, so once shipped they are fixed.
    inline constexpr auto ChromeDensityTable = std::array {
        ConfigEnumInfo<ChromeDensity> { ChromeDensity::Comfortable, "comfortable", "Comfortable" },
        ConfigEnumInfo<ChromeDensity> { ChromeDensity::Minimal, "minimal", "Minimal" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<ChromeDensity> const> configEnumValues() noexcept
{
    return detail::ChromeDensityTable;
}

/// The configuration token of @p density.
/// @param density The density.
/// @return `"comfortable"` or `"minimal"`.
[[nodiscard]] constexpr std::string_view chromeDensityName(ChromeDensity density) noexcept
{
    auto const values = configEnumValues<ChromeDensity>();
    for (auto const& row: values)
        if (row.value == density)
            return row.token;
    return values.front().token;
}

/// When a pane header shows its action buttons, short of the pointer being over it.
/// A layout token's type; zero is the default.
enum class PaneActionsReveal : uint8_t
{
    OnHover = 0,    //!< Only while the pointer is over the pane (or keyboard focus is on it).
    OnHoverOrFocus, //!< Also whenever the pane is the focused one.
};

/// A button on the pane header. Here rather than beside the header policy so the chrome's glyph table
/// (config::UiStyleTokens::paneActionGlyphs) is sized by the same count.
enum class PaneHeaderAction : uint8_t
{
    SplitRight = 0, //!< Split the pane side by side.
    SplitDown,      //!< Split the pane one above the other.
    Zoom,           //!< Zoom the pane to the whole tab.
    Close,          //!< Close the pane.
};

/// The number of PaneHeaderAction enumerators, derived from the last one.
inline constexpr size_t PaneHeaderActionCount = static_cast<size_t>(PaneHeaderAction::Close) + 1;

} // namespace contour::config
