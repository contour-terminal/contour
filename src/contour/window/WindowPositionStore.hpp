// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QMargins>
#include <QtCore/QPoint>
#include <QtCore/QRect>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace contour::window
{
/// State side file shared by the loader and window-close writer.
inline constexpr auto WindowPositionFileName = "window-position.yml";

/// Last normal-window location, in logical pixels relative to the monitor's available area.
struct WindowPosition
{
    QPoint position;
    int monitor = 0; ///< Zero-based index in Qt's monitor list.
};

/// Clamps the complete window frame to the target monitor's available desktop.
/// @param requested Remembered client top-left offset within the monitor's available area.
/// @param size Current client size, preserving configured rows and columns.
/// @param available The target monitor's available desktop area.
/// @param frameMargins Native frame outside the client area, in logical pixels.
/// @return A client position keeping the frame inside that area. Oversized frames align to its
///         leading edge so the title bar stays reachable without changing the configured grid.
[[nodiscard]] QPoint reachableWindowPosition(QPoint requested,
                                             QSize size,
                                             QRect available,
                                             QMargins frameMargins = {});

/// Persistence boundary for the last normal-window location; tests inject an in-memory store.
class WindowPositionStore
{
  public:
    virtual ~WindowPositionStore() = default;
    /// Reads a position, or no position on first use; reports malformed or unreadable files.
    /// @param path The state file beside the active configuration.
    [[nodiscard]] virtual std::expected<std::optional<WindowPosition>, std::string> load(
        std::filesystem::path const& path) const = 0;
    /// Atomically replaces the saved position.
    /// @param path The state file beside the active configuration.
    /// @param position The normal-window location to remember.
    [[nodiscard]] virtual std::expected<void, std::string> save(std::filesystem::path const& path,
                                                                WindowPosition const& position) = 0;
};

/// YAML side-file adapter using the shared atomic writer.
class FileWindowPositionStore final: public WindowPositionStore
{
  public:
    [[nodiscard]] std::expected<std::optional<WindowPosition>, std::string> load(
        std::filesystem::path const& path) const override;
    [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& path,
                                                        WindowPosition const& position) override;
};
} // namespace contour::window
