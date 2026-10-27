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

    /// @p wanted, no larger than @p screen leaves inside @p frame, and no smaller than @p minimum.
    [[nodiscard]] QSize fittedSize(QSize wanted,
                                   QSize minimum,
                                   QRect const& screen,
                                   QMargins const& frame) noexcept
    {
        return { std::max(minimum.width(),
                          std::min(wanted.width(), screen.width() - frame.left() - frame.right())),
                 std::max(minimum.height(),
                          std::min(wanted.height(), screen.height() - frame.top() - frame.bottom())) };
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
        {
            // A size remembered from a larger screen, or below today's minimum, fits as a default would.
            auto const size = fittedSize(previous.size(), placement.minimum, *best, placement.frame);
            return { reachableWindowPosition(
                         previous.topLeft() - best->topLeft(), size, *best, placement.frame),
                     size };
        }
    }

    auto const& a = placement.anchor;
    auto const& frame = placement.frame;
    auto const fitted = fittedSize(placement.preferred, placement.minimum, a, frame);
    auto const w = fitted.width();
    auto const h = fitted.height();
    return { reachableWindowPosition(
                 QPoint((a.width() - w) / 2, (a.height() - h) / 2), QSize(w, h), a, frame),
             QSize(w, h) };
}

} // namespace contour::window
