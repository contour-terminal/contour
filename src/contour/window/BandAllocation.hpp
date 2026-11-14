// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How the title band's room is shared between the location chip and the tab well: the chip gives way first,
/// down to its last step, before any tab loses its label; the tabs give way after it. One pure function that
/// returns the chip's fit and the well's layout together, so QML measures and asks once instead of repeating
/// the arithmetic.

#include <contour/window/LocationChip.hpp>
#include <contour/window/TabWellLayout.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace contour::window
{

/// The parts of the band that give way when its room runs short.
enum class BandPart : uint8_t
{
    LocationChip, ///< The chip: its steps, down to the workspace elided.
    TabLabels,    ///< The tabs' labels: the pills go icon-only.
    TabWell,      ///< The well itself: it scrolls.
};

/// One row of the yield order: which part gives way and the least width the well keeps once it has.
struct BandYield
{
    BandPart part; ///< The part that gives way at this rank.
    /// The well's least width when this part has given all it can: what the parts after it still need.
    double (*wellKeeps)(int tabCount, TabWellMetrics const& metrics) noexcept;
};

/// The parts in the order they give way. The chip goes first and the well keeps every label while it does;
/// then the labels go and the well keeps one icon-only pill; the well scrolls last and keeps nothing.
inline constexpr auto BandYieldOrder = std::array {
    BandYield { .part = BandPart::LocationChip, .wellKeeps = labelledTabWellMinimum },
    BandYield { .part = BandPart::TabLabels, .wellKeeps = iconOnlyTabWellMinimum },
    BandYield { .part = BandPart::TabWell,
                .wellKeeps = [](int, TabWellMetrics const&) noexcept { return 0.0; } },
};

/// What the room shared by the chip and the well comes to.
struct BandAllocation
{
    LocationChipFit chip = LocationChipFit::Full; ///< How much of the chip fits.
    double chipRoom = 0;                          ///< The room the chip had: what the well keeps is left out.
    double chipWidth = 0;                         ///< The width of the chip at that fit.
    TabWellLayout tabs {};                        ///< The tab well's layout in what the chip leaves.
};

/// @param widthOf   Gives the width of a chip step by its key (LocationChipSteps).
/// @param tabCount  The number of tabs.
/// @param metrics   The tab well's widths.
/// @param room      The width the band has for the chip and the well together, after its fixed parts, the
///                  margins and the drag reserve.
/// @param segments  What the chip names: a daemon-only chip takes fewer steps (locationChipTakes).
/// @return The chip's fit in what the well leaves it (the chip yields first), and the well's layout in what
///         the chip then leaves.
template <typename WidthOf>
[[nodiscard]] constexpr BandAllocation allocateBand(WidthOf const& widthOf,
                                                    int tabCount,
                                                    TabWellMetrics const& metrics,
                                                    double room,
                                                    ChipSegments segments = ChipSegments::DaemonAndWorkspace)
{
    auto const chipRoom = std::max(0.0, room - BandYieldOrder.front().wellKeeps(tabCount, metrics));
    auto const chip = locationChipFit(widthOf, chipRoom, segments);
    auto const step = std::ranges::find(LocationChipSteps, chip, &LocationChipStep::fit);
    auto const chipWidth = step != LocationChipSteps.end() ? widthOf(step->key) : 0.0;
    return BandAllocation { .chip = chip,
                            .chipRoom = chipRoom,
                            .chipWidth = chipWidth,
                            .tabs = layoutTabWell(std::max(0.0, room - chipWidth), tabCount, metrics) };
}

} // namespace contour::window
