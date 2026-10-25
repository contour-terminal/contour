// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/WindowLiveness.hpp>

#include <QtCore/QList>
#include <QtGui/QWindow>

#include <vector>

namespace contour::window
{

/// What the last-window rule needs to know about each of @p windows, for anyWindowKeepsAppAlive().
/// @param windows Top-level windows, as QGuiApplication::topLevelWindows() lists them.
/// @return One WindowFacts per window, in the same order.
[[nodiscard]] inline std::vector<WindowFacts> windowFactsOf(QList<QWindow*> const& windows)
{
    auto facts = std::vector<WindowFacts> {};
    facts.reserve(static_cast<std::size_t>(windows.size()));
    for (auto const* window: windows)
        facts.push_back(
            { .visibility = window->isVisible() ? WindowVisibility::Shown : WindowVisibility::Hidden,
              .transience = window->transientParent() != nullptr ? WindowTransience::Transient
                                                                 : WindowTransience::Independent });
    return facts;
}

} // namespace contour::window
