// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/HitMap.hpp>

#include <ranges>
#include <vector>

namespace orchestrator::view
{

void HitMap::add(core::tui::Rect area, Hit hit)
{
    if (!area.empty())
        _regions.push_back(HitRegion { .area = area, .hit = hit });
}

std::vector<Hit> HitMap::hitsAt(int x, int y) const
{
    return std::ranges::to<std::vector>(regionsAt(x, y) | std::views::transform(&HitRegion::hit));
}

std::vector<HitRegion> HitMap::regionsAt(int x, int y) const
{
    return std::ranges::to<std::vector>(
        _regions | std::views::reverse
        | std::views::filter([&](HitRegion const& region) { return region.area.contains(x, y); }));
}

} // namespace orchestrator::view
