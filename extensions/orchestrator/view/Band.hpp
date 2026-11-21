// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

namespace orchestrator::view
{

/// How wide the dashboard's tab is, which decides how the list and the details share it.
enum class Band : uint8_t
{
    Wide,   ///< At least WideFromColumns: the list and the details side by side.
    Medium, ///< MediumFromColumns to WideFromColumns - 1: the list, the details below it or swapped in with
            ///< Tab.
    Narrow, ///< Below MediumFromColumns: the list only; Enter opens the details as a page, Esc returns.
};

/// The narrowest tab the Wide band is chosen at.
inline constexpr int WideFromColumns = 100;
/// The narrowest tab the Medium band is chosen at.
inline constexpr int MediumFromColumns = 60;
/// The lowest tab that shows the key-hint line and the section headings; below it they collapse first.
inline constexpr int CollapseBelowRows = 20;

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

/// Whether the dashboard shows what only explains it: the key-hint line, the section headings and the
/// placeholders of empty sections.
enum class Furniture : uint8_t
{
    Collapsed, ///< Left out: the tab is lower than CollapseBelowRows.
    Shown,     ///< Shown.
};

/// One layout the dashboard is built in: the smallest tab it is chosen for, and what it shows.
struct BandBreakpoint
{
    int minColumns = 0;                     ///< The narrowest tab it is chosen at.
    int minRows = 0;                        ///< The lowest tab it is chosen at.
    Band band = Band::Narrow;               ///< How the list and the details share the tab.
    Furniture furniture = Furniture::Shown; ///< Whether the hints and headings are shown.
};

/// The dashboard's layouts, narrowest first: core::tui::Responsive chooses the last one a size meets.
inline constexpr auto BandBreakpoints = std::array {
    BandBreakpoint { .minColumns = 0, .minRows = 0, .band = Band::Narrow, .furniture = Furniture::Collapsed },
    BandBreakpoint {
        .minColumns = 0, .minRows = CollapseBelowRows, .band = Band::Narrow, .furniture = Furniture::Shown },
    BandBreakpoint { .minColumns = MediumFromColumns,
                     .minRows = 0,
                     .band = Band::Medium,
                     .furniture = Furniture::Collapsed },
    BandBreakpoint { .minColumns = MediumFromColumns,
                     .minRows = CollapseBelowRows,
                     .band = Band::Medium,
                     .furniture = Furniture::Shown },
    BandBreakpoint {
        .minColumns = WideFromColumns, .minRows = 0, .band = Band::Wide, .furniture = Furniture::Collapsed },
    BandBreakpoint { .minColumns = WideFromColumns,
                     .minRows = CollapseBelowRows,
                     .band = Band::Wide,
                     .furniture = Furniture::Shown },
};

} // namespace orchestrator::view
