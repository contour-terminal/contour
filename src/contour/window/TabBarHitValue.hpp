// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/TabBarDropPolicy.hpp>

#include <QtCore/QObject>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <utility>

/// The tab-strip hit test as QML reads it. Its own namespace, because Q_ENUM_NS needs Q_NAMESPACE in the
/// namespace that declares the enum, and TabBarDropPolicy.hpp -- where the decision lives -- stays Qt-free;
/// the enum here mirrors that one value for value. QML names it as TabBarSpot.OnTab, ... once
/// registerTabBarQmlTypes() has run.
namespace contour::window::tabbar
{
Q_NAMESPACE

/// What a pointer over the tab strip is on (@see window::TabBarSpot, which it mirrors).
enum class TabBarSpot : uint8_t
{
    None = 0, ///< Nothing.
    OnTab,    ///< Inside a tab.
    Gap,      ///< In the gap between two tabs, or before the first or after the last.
};
Q_ENUM_NS(TabBarSpot)

static_assert(std::to_underlying(TabBarSpot::None) == std::to_underlying(window::TabBarSpot::None));
static_assert(std::to_underlying(TabBarSpot::OnTab) == std::to_underlying(window::TabBarSpot::OnTab));
static_assert(std::to_underlying(TabBarSpot::Gap) == std::to_underlying(window::TabBarSpot::Gap));

} // namespace contour::window::tabbar

namespace contour::window
{

/// A tab-strip hit (window::TabBarHit) as a QML value: what the pointer is on, and which tab or gap.
class TabBarHitValue
{
    Q_GADGET
    QML_VALUE_TYPE(tabBarHit)
    Q_PROPERTY(contour::window::tabbar::TabBarSpot spot READ spot CONSTANT)
    Q_PROPERTY(int index READ index CONSTANT)

  public:
    /// No hit at all. A value type must default-construct for QML (framework-mandated), and None is the
    /// hit of nothing.
    TabBarHitValue() = default;

    /// @param hit The hit the strip's policy decided.
    explicit TabBarHitValue(TabBarHit hit) noexcept: _hit(hit) {}

    /// @return What the pointer is on.
    [[nodiscard]] tabbar::TabBarSpot spot() const noexcept
    {
        return static_cast<tabbar::TabBarSpot>(std::to_underlying(_hit.spot));
    }

    /// @return OnTab: the tab's index; Gap: the insertion index; None: 0.
    [[nodiscard]] int index() const noexcept { return _hit.index; }

    /// @return The hit as the policy states it.
    [[nodiscard]] TabBarHit hit() const noexcept { return _hit; }

  private:
    TabBarHit _hit {};
};

/// Registers the drag vocabulary with QML under Contour.Terminal: TabBarSpot, and the NodeDrag singleton
/// whose `mime` is NodeDragMime. The first call registers; later ones return.
void registerTabBarQmlTypes();

} // namespace contour::window
