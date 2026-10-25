// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsWindowRules.hpp>
#include <contour/window/WindowPositionStore.hpp>

#include <algorithm>

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
        QRect const* best = nullptr;
        auto bestArea = qint64 { 0 };
        for (auto const& screen: placement.screens)
            if (auto const area = intersectionArea(previous, screen); area > bestArea)
            {
                best = &screen;
                bestArea = area;
            }
        if (best != nullptr)
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
