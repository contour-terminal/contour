// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/DesktopButtonLayout.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/platform/NativeWindowFrame.hpp>
#include <contour/platform/TitleBandMaterial.hpp>
#include <contour/window/SettingsBackdrop.hpp>
#include <contour/window/SettingsPreview.hpp>
#include <contour/window/SettingsWindowRules.hpp>

#include <QtCore/QObject>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/QVariantList>
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

/// What dresses the Settings window's frame: the OS's own title bar (R-m), which on the Fluent row shows
/// Mica and follows the chrome's light or dark; or, beside a Linux header bar, a frame the window draws
/// itself, with the desktop's close button in its header row (R-l).
struct SettingsWindowDressing
{
    platform::NativeWindowFrame& frame;     ///< Decorates the window; borrowed, outlives the host.
    platform::ITitleBandMaterial& material; ///< Draws the backdrop; borrowed, outlives the host.
    /// What the backdrop is decided from, read whenever the window is dressed. Called on the GUI thread.
    std::function<SettingsBackdropInputs()> inputs;
    /// Who draws the window's frame (settingsWindowDecoration). Fixed for the host's life: the frameless hint
    /// is read when the window is built, and swapping it recreates the platform window.
    platform::WindowDecoration decoration = platform::WindowDecoration::Server;
    /// Where the desktop puts the window buttons now (config::windowButtonPlacement), read when the host is
    /// built and by refreshWindowControls(); null puts none anywhere. Called on the GUI thread.
    std::function<config::WindowButtonLayout()> placement;
};

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
    /// The terminal the page previews the open profile in; null when the host was built without a way to make
    /// its session (the snapshot scene's).
    Q_PROPERTY(contour::window::SettingsPreview* preview READ preview CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(Qt::WindowFlags windowFlags READ windowFlags CONSTANT)
    /// The page the window shows, as the page reports it: remembered when the window closes and shown again
    /// when it opens after a restart. Domain state the window reports, not configuration, hence the setter.
    Q_PROPERTY(QString lastPage READ lastPage WRITE setLastPage NOTIFY lastPageChanged)
    /// Whether the window draws its own frame: a resize border, its header row as the title bar, and the
    /// window controls below. A QML-facing bool, read once when the window is built.
    Q_PROPERTY(bool frameless READ frameless CONSTANT)
    /// The window controls the frameless window draws at its header row's leading end, as
    /// config::WindowControlKind values in drawing order; empty while the OS draws the frame.
    Q_PROPERTY(QVariantList leadingControls READ leadingControls NOTIFY windowControlsChanged)
    /// The window controls at the header row's trailing end; as leadingControls.
    Q_PROPERTY(QVariantList trailingControls READ trailingControls NOTIFY windowControlsChanged)

  public:
    /// @param controller  What the page edits; borrowed, outlives this host.
    /// @param conventions The title and title-bar buttons the window presents itself with.
    /// @param factory     Builds the window on the first show.
    /// @param dressing    The frame and backdrop the window is dressed with on every expose and palette
    /// change.
    /// @param state       What the window remembers across restarts: its frame, screen and page. Borrowed:
    ///                    it outlives this host.
    /// @param previewFactory Makes the preview terminal's session; null: the host has no `preview`.
    /// @param editConfiguration Opens contour.yml in the OS's editor: what the sidebar's "Edit contour.yml…"
    ///                    row runs. Called on the GUI thread.
    /// @param openInNewTab Opens a tab running the named profile in the terminal window the app would use for
    ///                    a command of its own: what a profile page's "Open in new tab" runs. Called on the
    ///                    GUI thread.
    /// @param parent      Qt ownership.
    SettingsWindowHost(SettingsController& controller,
                       SettingsWindowConventions conventions,
                       SettingsWindowFactory factory,
                       SettingsWindowDressing dressing,
                       config::GuiStateStore& state,
                       SettingsPreview::SessionFactory previewFactory,
                       std::function<void()> editConfiguration,
                       std::function<void(std::string const&)> openInNewTab,
                       QObject* parent = nullptr);
    ~SettingsWindowHost() override;

    SettingsWindowHost(SettingsWindowHost const&) = delete;
    SettingsWindowHost& operator=(SettingsWindowHost const&) = delete;
    SettingsWindowHost(SettingsWindowHost&&) = delete;
    SettingsWindowHost& operator=(SettingsWindowHost&&) = delete;

    /// @return The controller the page edits.
    [[nodiscard]] SettingsController* controller() const noexcept { return &_controller; }

    /// @return The preview terminal; null when the host has no way to make its session.
    [[nodiscard]] SettingsPreview* preview() const noexcept { return _preview.get(); }

    /// @return The window title of the host's conventions.
    [[nodiscard]] QString title() const;

    /// @return settingsWindowFlags() of the conventions' buttons, for SettingsWindow.qml's `flags`; a
    ///         frameless window's are those of a window without a frame.
    [[nodiscard]] Qt::WindowFlags windowFlags() const noexcept;

    /// @return Whether the window draws its own frame: the dressing's decoration is Client.
    [[nodiscard]] bool frameless() const noexcept
    {
        return _dressing.decoration == platform::WindowDecoration::Client;
    }

    /// @return The window controls at the header row's leading end, as config::WindowControlKind values.
    [[nodiscard]] QVariantList leadingControls() const;
    /// @return The window controls at the header row's trailing end, as config::WindowControlKind values.
    [[nodiscard]] QVariantList trailingControls() const;

    /// Reads the desktop's button placement again, as it changes live: the frameless window's controls are
    /// the conventions' buttons where it puts them (settingsWindowControls). Emits windowControlsChanged()
    /// on a change.
    void refreshWindowControls();

    /// Builds the window on the first call; a hidden window then has the controller refreshed, is placed
    /// (R-c) unless it is maximised or full screen, whose geometry is left alone, and shown in the state it
    /// closed in, while a visible one is left as it is, mid-edit draft included. Either way a minimised
    /// state is cleared (keeping maximised), the window is dressed, and it is raised and activated. The first
    /// show of a run places the window over the frame it closed at in the run before, on the screen it was on
    /// when that screen is there, maximised if it was (GuiState::settingsWindow).
    /// @param anchor The screen of the window that asked; null for the primary screen.
    /// @return Nothing, or why the window could not be built. A failed build keeps no window, and the next
    ///         call tries again.
    /// @pre GUI thread.
    [[nodiscard]] std::expected<void, SettingsWindowError> show(QScreen* anchor);

    /// Closes the window as its title bar's close button does: the page's Close button and the Close
    /// shortcut. What Settings has staged is written first, as it is whenever the window hides. The window
    /// object, its page and the draft survive; only the platform window goes, so Qt's quit-on-last-window
    /// rule sees the close. The close is a no-op before the first show.
    Q_INVOKABLE void closeSettings();

    /// Opens contour.yml in the OS's editor, as the sidebar's "Edit contour.yml…" row asks: runs the
    /// injected `editConfiguration`. What Settings has staged is not written first; the file it opens is the
    /// hand-maintained one, which Settings never writes.
    Q_INVOKABLE void editConfigurationFile();

    /// Opens a tab running profile @p name, as a profile page's "Open in new tab" asks: runs the injected
    /// `openInNewTab`.
    /// @param name The profile to run.
    Q_INVOKABLE void openProfileInNewTab(QString const& name);

    /// @return The window, or null before the first successful show.
    [[nodiscard]] QQuickWindow* window() const noexcept { return _window.get(); }

    /// @return The page the window shows, as last reported; the remembered one before the first show.
    [[nodiscard]] QString lastPage() const { return _lastPage; }

    /// Records the page the window shows. Emits lastPageChanged() on a change.
    /// @param page The page's id.
    void setLastPage(QString const& page);

    /// Asks the window to show @p page: remembered as lastPage() for a window not built yet, and signalled
    /// through pageRequested() to one that is. A page the window does not know leaves it where it was.
    /// @param page The page's id: `section:hosts`, `host:NAME`.
    void requestPage(QString const& page);

    /// Dresses the window on its expose and on a palette change (the chrome's light or dark may have
    /// flipped).
    /// @param watched The object the event is for.
    /// @param event   The event.
    /// @return False: the event goes on to the window.
    bool eventFilter(QObject* watched, QEvent* event) override;

  signals:
    /// lastPage() changed.
    void lastPageChanged();
    /// requestPage() asked for @p page; the built window opens it.
    void pageRequested(QString const& page);
    /// leadingControls() or trailingControls() changed.
    void windowControlsChanged();

  private:
    /// Applies the dressing's frame, and the backdrop the inputs ask for when it differs from the one
    /// applied.
    void dress();

    /// @return The controls the frameless window draws now: none while the OS draws the frame.
    [[nodiscard]] config::WindowButtonLayout currentWindowControls() const;

    /// What a hidden window leaves behind: what Settings staged is written, and the frame, screen and page
    /// are remembered.
    void onHidden();

    /// Records the frame the window has while neither maximised nor full screen.
    void trackNormalGeometry();
    /// Moves the hidden window to where settingsWindowGeometry() puts it.
    /// @param anchor   The screen to open on; null for the primary screen.
    /// @param previous Where the window was last shown; none when this show built it.
    void place(QScreen* anchor, std::optional<QRect> previous);

    SettingsController& _controller;
    SettingsWindowConventions _conventions;
    SettingsWindowFactory _factory;
    SettingsWindowDressing _dressing;
    config::GuiStateStore& _state;
    std::function<void()> _editConfiguration;
    std::function<void(std::string const&)> _openInNewTab;
    std::unique_ptr<SettingsPreview> _preview; ///< Null without a preview session factory.
    /// What the run before left, read once at construction; consumed by the first show.
    std::optional<config::SettingsWindowState> _remembered;
    QString _lastPage;
    /// The frame the window last had while neither maximised nor full screen: the one it returns to.
    std::optional<QRect> _normalGeometry;
    /// The backdrop request last applied successfully; none before the first, or after it was withdrawn.
    std::optional<platform::TitleBandRequest> _appliedBackdrop;
    /// Once the material failed, the window asks for none again (R-g).
    platform::BandMaterialHealth _backdropHealth = platform::BandMaterialHealth::Working;
    config::WindowButtonLayout _windowControls; ///< What leadingControls() and trailingControls() publish.
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
