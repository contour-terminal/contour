// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/geometry/WindowGeometry.hpp>

#include <QtCore/QObject>

#include <functional>
#include <memory>
#include <optional>

class QWindow;

namespace contour::platform
{

/// Installs a window-scoped native resize subscription on Windows or macOS.
/// Must be created and destroyed on the window's GUI thread.
/// @param window The window to constrain; no native handle is created early.
/// @param grid Supplies current geometry, or nullopt when snapping is disabled or inapplicable.
///             Queried for each resize so configuration reloads and font/DPI changes take effect.
/// Windows constrains each interactive proposal; macOS aligns the window when live resizing ends.
/// X11 uses size hints instead; Wayland has no resize-increment hint.
/// @return An owning subscription, or nullptr where no subscription is required.
[[nodiscard]] std::unique_ptr<QObject> makeWindowResizeGrid(
    QWindow& window, std::function<std::optional<geometry::RowResizeGrid>()> grid);

/// Refines Qt's X11 size hints with the physical row metrics, preserving all unrelated hints.
/// Call after applying Qt's ordinary hints; does not create a native window early.
/// @param window The window whose hints were just updated.
/// @param grid Current physical row metrics, or nullopt to restore Qt's ordinary hints.
void applyRowResizeHints(QWindow& window, std::optional<geometry::RowResizeGrid> grid);

#ifdef __APPLE__
/// Creates the AppKit live-resize subscription, only when using Qt's Cocoa platform plugin.
/// @param window The window to align when manual resizing ends.
/// @param grid Supplies current physical row metrics or nullopt when disabled.
/// @return An owning notification subscription, or nullptr on a non-Cocoa Qt platform.
[[nodiscard]] std::unique_ptr<QObject> makeCocoaWindowResizeGrid(
    QWindow& window, std::function<std::optional<geometry::RowResizeGrid>()> grid);
#endif

} // namespace contour::platform
