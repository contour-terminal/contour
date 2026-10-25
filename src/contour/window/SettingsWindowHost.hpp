// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/SettingsWindowRules.hpp>

#include <QtCore/QObject>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtQuick/QQuickWindow>

#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

class QQmlEngine;
class QScreen;

namespace contour::window
{

/// Why the Settings window could not be shown.
enum class SettingsWindowError : uint8_t
{
    Unavailable = 0, //!< No QML engine to build it on (a headless app).
    LoadFailed,      //!< SettingsWindow.qml did not load; the component's errors were logged.
};

/// The enumerator's name of @p error, for a log line.
/// @param error The error.
/// @return Its name.
[[nodiscard]] constexpr std::string_view nameOf(SettingsWindowError error) noexcept
{
    switch (error)
    {
        case SettingsWindowError::Unavailable: return "Unavailable";
        case SettingsWindowError::LoadFailed: return "LoadFailed";
    }
    return "Unknown";
}

class SettingsController;

class SettingsWindowHost;

/// Builds the Settings window for @p host; called once, on the first show.
using SettingsWindowFactory = std::function<std::expected<std::unique_ptr<QQuickWindow>, SettingsWindowError>(
    SettingsWindowHost& host)>;

/// The app's one Settings window: built on first show, and kept (never destroyed) when it closes.
///
/// It is what SettingsWindow.qml binds to as `host`: the controller the page edits, the title and the
/// title-bar buttons of the host's conventions, and closeSettings() for the page's Close button and the
/// Close shortcut.
class SettingsWindowHost final: public QObject
{
    Q_OBJECT
    // The `controller` property's type, which moc's code needs complete; this header only names it.
    Q_MOC_INCLUDE("contour/window/SettingsController.hpp")
    Q_PROPERTY(contour::window::SettingsController* controller READ controller CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(Qt::WindowFlags windowFlags READ windowFlags CONSTANT)

  public:
    /// @param controller  What the page edits; borrowed, outlives this host.
    /// @param conventions The title and title-bar buttons the window presents itself with.
    /// @param factory     Builds the window on the first show.
    /// @param parent      Qt ownership.
    SettingsWindowHost(SettingsController& controller,
                       SettingsWindowConventions conventions,
                       SettingsWindowFactory factory,
                       QObject* parent = nullptr);
    ~SettingsWindowHost() override;

    SettingsWindowHost(SettingsWindowHost const&) = delete;
    SettingsWindowHost& operator=(SettingsWindowHost const&) = delete;
    SettingsWindowHost(SettingsWindowHost&&) = delete;
    SettingsWindowHost& operator=(SettingsWindowHost&&) = delete;

    /// @return The controller the page edits.
    [[nodiscard]] SettingsController* controller() const noexcept { return &_controller; }

    /// @return The window title of the host's conventions.
    [[nodiscard]] QString title() const;

    /// @return settingsWindowFlags() of the conventions' buttons, for SettingsWindow.qml's `flags`.
    [[nodiscard]] Qt::WindowFlags windowFlags() const noexcept;

    /// Builds the window on the first call; a hidden window then has the controller refreshed, is placed
    /// (R-c) and shown in the state it closed in, while a visible one is left as it is, mid-edit draft
    /// included. Either way a minimised state is cleared (keeping maximised), and the window is raised and
    /// activated.
    /// @param anchor The screen of the window that asked; null for the primary screen.
    /// @return Nothing, or why the window could not be built. A failed build keeps no window, and the next
    ///         call tries again.
    /// @pre GUI thread.
    [[nodiscard]] std::expected<void, SettingsWindowError> show(QScreen* anchor);

    /// Closes the window as its title bar's close button does: the page's Close button and the Close
    /// shortcut. The window object, its page and the draft survive; only the platform window goes, so Qt's
    /// quit-on-last-window rule sees the close. A no-op before the first show.
    Q_INVOKABLE void closeSettings();

    /// @return The window, or null before the first successful show.
    [[nodiscard]] QQuickWindow* window() const noexcept { return _window.get(); }

  private:
    /// Moves the hidden window to where settingsWindowGeometry() puts it.
    /// @param anchor   The screen to open on; null for the primary screen.
    /// @param previous Where the window was last shown; none when this show built it.
    void place(QScreen* anchor, std::optional<QRect> previous);

    SettingsController& _controller;
    SettingsWindowConventions _conventions;
    SettingsWindowFactory _factory;
    std::unique_ptr<QQuickWindow> _window;
};

/// Loads Contour.Ui's SettingsWindow on @p engine with `host` as its initial property.
/// @param engine The engine to build it on; it must outlive the window.
/// @param host   The host the window binds to.
/// @return The window, or LoadFailed when the component is not ready or makes no QQuickWindow.
[[nodiscard]] std::expected<std::unique_ptr<QQuickWindow>, SettingsWindowError> loadSettingsWindow(
    QQmlEngine& engine, SettingsWindowHost& host);

/// The window flags of the Settings window's title-bar buttons.
/// @param buttons The buttons.
/// @return CloseOnly: Window | WindowTitleHint | WindowCloseButtonHint | CustomizeWindowHint; All: Window.
[[nodiscard]] Qt::WindowFlags settingsWindowFlags(SettingsWindowButtons buttons) noexcept;

} // namespace contour::window

/// Formats a SettingsWindowError as its enumerator's name.
template <>
struct std::formatter<contour::window::SettingsWindowError>: std::formatter<std::string_view>
{
    auto format(contour::window::SettingsWindowError error, auto& ctx) const
    {
        return std::formatter<std::string_view>::format(contour::window::nameOf(error), ctx);
    }
};
