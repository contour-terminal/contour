// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/WindowLiveness.hpp>

#include <QtCore/QList>
#include <QtGui/QWindow>

#include <vector>

namespace contour::window
{

/// What the last-window rule needs to know about each of @p windows, for anyWindowKeepsAppAlive().
/// @param windows         Top-level windows, as QGuiApplication::topLevelWindows() lists them.
/// @param panel           A window whose visibility is not what Qt says it is: the quick terminal, which
///                        a platform may keep "visible" while it is parked off screen. Null for none.
/// @param panelVisibility What @p panel counts as.
/// @return One WindowFacts per window, in the same order.
[[nodiscard]] inline std::vector<WindowFacts> windowFactsOf(
    QList<QWindow*> const& windows,
    QWindow const* panel = nullptr,
    WindowVisibility panelVisibility = WindowVisibility::Hidden)
{
    auto facts = std::vector<WindowFacts> {};
    facts.reserve(static_cast<std::size_t>(windows.size()));
    for (auto const* window: windows)
    {
        auto const qtVisibility = window->isVisible() ? WindowVisibility::Shown : WindowVisibility::Hidden;
        facts.push_back({ .visibility = window == panel ? panelVisibility : qtVisibility,
                          .transience = window->transientParent() != nullptr
                                            ? WindowTransience::Transient
                                            : WindowTransience::Independent });
    }
    return facts;
}

} // namespace contour::window
