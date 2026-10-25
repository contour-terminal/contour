// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsWindowRules.hpp>
#include <contour/window/WindowPositionStore.hpp>

#include <algorithm>
#include <functional>

namespace contour::window
{

namespace
{
    /// @return The area of the intersection of @p a and @p b, 0 when they do not meet.
    [[nodiscard]] qint64 intersectionArea(QRect const& a, QRect const& b) noexcept
    {
        auto const common = a.intersected(b);
        return common.isEmpty() ? 0 : qint64 { common.width() } * common.height();
    }
} // namespace

QRect settingsWindowGeometry(SettingsWindowPlacement const& placement)
{
    if (placement.previous)
    {
        auto const& previous = *placement.previous;
        auto const overlap = [&previous](QRect const& screen) {
            return intersectionArea(previous, screen);
        };
        // The first of the screens it overlaps most, as long as it overlaps one at all.
        if (auto const best = std::ranges::max_element(placement.screens, std::ranges::less {}, overlap);
            best != placement.screens.end() && overlap(*best) > 0)
            return { reachableWindowPosition(
                         previous.topLeft() - best->topLeft(), previous.size(), *best, placement.frame),
                     previous.size() };
    }

    auto const& a = placement.anchor;
    auto const& frame = placement.frame;
    auto const w = std::max(placement.minimum.width(),
                            std::min(placement.preferred.width(), a.width() - frame.left() - frame.right()));
    auto const h =
        std::max(placement.minimum.height(),
                 std::min(placement.preferred.height(), a.height() - frame.top() - frame.bottom()));
    return { reachableWindowPosition(
                 QPoint((a.width() - w) / 2, (a.height() - h) / 2), QSize(w, h), a, frame),
             QSize(w, h) };
}

} // namespace contour::window
