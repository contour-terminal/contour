// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How wide the pills of the tab well are: equal width up to the maximum, icon-only below the minimum,
/// and a scrolling well below the icon-only width (R-f). Pure and Qt-free.

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace contour::window
{

/// What a pill shows.
enum class TabLabelMode : uint8_t
{
    Full = 0, ///< Icon and label.
    IconOnly, ///< The icon alone (or the badge in its place).
};

/// Whether the pills fit the well.
enum class TabWellOverflow : uint8_t
{
    Fits = 0, ///< Every pill is in view.
    Scrolls,  ///< The well scrolls, keeping the active tab in view.
};

/// The widths the well is laid out with.
struct TabWellMetrics
{
    double minTab = 0;      ///< The narrowest pill that keeps its label.
    double maxTab = 0;      ///< The widest pill.
    double iconOnlyTab = 0; ///< The width of an icon-only pill, and the narrowest pill at all.
    double gap = 0;         ///< The gap between two pills.
    double padding = 0;     ///< The padding at each end of the well.
};

/// The well's layout.
struct TabWellLayout
{
    double tabWidth = 0;                              ///< Every pill's width.
    double wellWidth = 0;                             ///< The well's visible width.
    TabLabelMode label = TabLabelMode::Full;          ///< What the pills show.
    TabWellOverflow overflow = TabWellOverflow::Fits; ///< Whether the well scrolls.

    bool operator==(TabWellLayout const&) const = default;
};

/// Lays out the tab well.
/// @param available The width the band leaves the well; a negative width counts as zero.
/// @param tabCount  The number of tabs; zero or fewer gives the default layout.
/// @param metrics   The widths to lay out with.
/// @return Equal pills of min(maxTab, share) while the share reaches minTab, icon-only pills at the share
///         while it reaches iconOnlyTab, else icon-only pills at iconOnlyTab in a scrolling well.
[[nodiscard]] constexpr TabWellLayout layoutTabWell(double available,
                                                    int tabCount,
                                                    TabWellMetrics const& metrics) noexcept
{
    if (tabCount <= 0)
        return {};
    auto const n = static_cast<double>(tabCount);
    auto const width = std::max(0.0, available);
    auto const share = (width - (2 * metrics.padding) - ((n - 1) * metrics.gap)) / n;

    auto layout = TabWellLayout {};
    if (share >= metrics.minTab)
        layout.tabWidth = std::min(metrics.maxTab, share);
    else if (share >= metrics.iconOnlyTab)
        layout = { .tabWidth = share, .label = TabLabelMode::IconOnly };
    else
        layout = { .tabWidth = metrics.iconOnlyTab,
                   .label = TabLabelMode::IconOnly,
                   .overflow = TabWellOverflow::Scrolls };
    layout.wellWidth =
        std::min(width, (2 * metrics.padding) + (n * layout.tabWidth) + ((n - 1) * metrics.gap));
    return layout;
}

/// The name QML reads a label mode by.
/// @param mode The mode.
/// @return "full" or "iconOnly".
[[nodiscard]] constexpr std::string_view tabLabelModeName(TabLabelMode mode) noexcept
{
    switch (mode)
    {
        case TabLabelMode::Full: return "full";
        case TabLabelMode::IconOnly: return "iconOnly";
    }
    return "full";
}

} // namespace contour::window
