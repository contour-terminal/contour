// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

namespace orchestrator::view
{

/// How wide the dashboard's tab is, which decides how the Fleet and the Focus share it.
enum class Band : uint8_t
{
    Wide,   ///< At least WideFromColumns: the Fleet and the Focus side by side.
    Medium, ///< MediumFromColumns to WideFromColumns - 1: the Fleet above the Focus; the Focus takes the body
            ///< while it has the keys.
    Narrow, ///< Below MediumFromColumns: the Fleet or the Focus, one at a time.
};

/// The narrowest tab the Wide band is chosen at.
inline constexpr int WideFromColumns = 100;
/// The narrowest tab the Medium band is chosen at.
inline constexpr int MediumFromColumns = 60;

/// @param columns The tab's width.
/// @return The band a tab of that width is in.
[[nodiscard]] constexpr Band bandOf(int columns) noexcept
{
    if (columns >= WideFromColumns)
        return Band::Wide;
    if (columns >= MediumFromColumns)
        return Band::Medium;
    return Band::Narrow;
}

/// One layout the dashboard is built in: the narrowest tab it is chosen for, and its band.
struct BandBreakpoint
{
    int minColumns = 0;       ///< The narrowest tab it is chosen at.
    Band band = Band::Narrow; ///< How the Fleet and the Focus share the tab.
};

/// The dashboard's layouts, narrowest first: core::tui::Responsive chooses the last one a size meets.
inline constexpr auto BandBreakpoints = std::array {
    BandBreakpoint { .minColumns = 0, .band = Band::Narrow },
    BandBreakpoint { .minColumns = MediumFromColumns, .band = Band::Medium },
    BandBreakpoint { .minColumns = WideFromColumns, .band = Band::Wide },
};

} // namespace orchestrator::view
