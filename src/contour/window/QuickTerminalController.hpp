// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/HotkeyChord.hpp>
#include <contour/config/QuickTerminalConfig.hpp>
#include <contour/platform/GlobalHotkey.hpp>
#include <contour/platform/QuickTerminalPanel.hpp>
#include <contour/window/QuickTerminalPolicy.hpp>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QPropertyAnimation>
#include <QtGui/QRegion>
#include <QtGui/QWindow>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace contour::window
{

/// What the quick terminal controller reaches the world through: every OS service and every decision that is
/// not its own. Fixed for the controller's life.
struct QuickTerminalHost
{
    /// The application-wide hotkey the controller registers its chord on.
    platform::IGlobalHotkey& hotkey;
    /// The native side of the window.
    platform::IQuickTerminalPanel& panel;
    /// Creates the quick terminal's window, Minimal and with its first tab; null when it could not be made.
    /// The window is not shown. GUI thread.
    std::function<QWindow*()> createWindow;
    /// The available geometry of the screen under the pointer, in screen pixels (R-b).
    std::function<ScreenRect()> screenUnderPointer;
    /// The full geometry of every screen but the one screenUnderPointer names, in screen pixels: what the
    /// hidden frame must stay off. Null where there is no other screen.
    std::function<std::vector<ScreenRect>()> otherScreens {};
    /// Told why the hotkey could not be registered, on the GUI thread, once per refusal.
    std::function<void(platform::HotkeyError)> onHotkeyError;
    /// Told that the configured hotkey is registered, on the GUI thread, so a problem shown for an earlier
    /// refusal can go. Null where nobody shows one.
    std::function<void()> onHotkeyBound {};
    /// Told, on the GUI thread, that the quick terminal is off the screen for now (Hidden) or for good
    /// (Absent): the app may have nothing left to keep it running (ruling D1). Null where nobody asks.
    std::function<void()> onOffScreen {};
    /// Whether a chord belongs to the menu bar and so can never be the hotkey (R-f).
    std::function<bool(config::HotkeyChord const&)> isMenuOwned;
    /// The radius of the window's bottom corners in pixels (`radii.quickTerminal`); 0 leaves them square.
    std::function<int()> cornerRadius;
    /// Who places windows: a Wayland client cannot, so its quick terminal fades in place.
    PlacementControl placement = PlacementControl::Client;
};

/// The shape of a quick terminal window: its rectangle with the bottom corners rounded.
///
/// Applied with QWindow::setMask, which the X11 and Windows platform plugins honour and Wayland and Cocoa
/// do not (there the corners stay square): the application's compositing forbids the render-target layer a
/// QML-side clip would need, because the terminal paints over the scene graph.
/// @param width  The window's width.
/// @param height The window's height.
/// @param radius The corner radius; limited to what the window's size allows.
/// @return The region of the window that is inside the shape; the whole rectangle for a radius of 0.
[[nodiscard]] QRegion roundedBottomMask(int width, int height, int radius);

/// Runs the quick terminal's life: registers its hotkey, creates its one window on the first press, and
/// slides or fades it in and out as the transition table (quickTerminalStep) says. GUI thread only.
///
/// There is one quick terminal per app, so at most one window. A press while the window is moving reverses
/// it; focus loss hides it unless the configuration says to stay; the window closing (its last tab ended)
/// returns the controller to Absent, and the next press makes a new one.
class QuickTerminalController: public QObject
{
  public:
    /// Registers the configured hotkey, if there is one.
    /// @param host   The collaborators; must outlive the controller.
    /// @param config The quick terminal's configuration.
    /// @param motion The motion tokens the animation takes its duration and style from.
    /// @param parent The QObject parent.
    QuickTerminalController(QuickTerminalHost host,
                            config::QuickTerminalConfig config,
                            config::MotionTokens motion,
                            QObject* parent = nullptr);

    /// Applies a changed configuration.
    ///
    /// Live reconfiguration, because the user edits the file or accessibility changes while the app runs and
    /// the controller owns a window and a registered chord that outlive any one configuration: the hotkey is
    /// registered again only when the chord differs, and the focus-loss rule and the motion apply from the
    /// next transition on.
    /// @param config The new configuration.
    /// @param motion The new motion tokens.
    void reconfigure(config::QuickTerminalConfig config, config::MotionTokens motion);

    /// @return Where the quick terminal's life stands.
    [[nodiscard]] QuickTerminalState state() const noexcept { return _state; }

    /// @return The quick terminal's window, or null while there is none.
    [[nodiscard]] QWindow* window() const noexcept { return _window.data(); }

    /// What the hotkey does: shows, hides or focuses the quick terminal, as the state says.
    /// @param activationToken The desktop's focus-stealing token; empty where there is none.
    void press(std::string activationToken);

  private:
    void applyHotkey();
    void dispatch(QuickTerminalEvent event);
    void perform(QuickTerminalEffect effect);
    void noteOffScreen() const;
    void createAndShow();
    void showWindow();
    void startSlide(QAbstractAnimation::Direction direction);
    void onAnimationFinished();
    void replaceOnScreenChange(QScreen* screen);
    void relayoutOn(ScreenRect available);
    [[nodiscard]] QuickTerminalFocus focus() const;

    QuickTerminalHost _host;
    config::QuickTerminalConfig _config;
    config::MotionTokens _motion;
    /// The chord handed to the hotkey and not refused; absent when none is, and again once the desktop
    /// refuses the chord, so the next reconfiguration tries it afresh.
    std::optional<config::HotkeyChord> _registered {};
    QuickTerminalState _state = QuickTerminalState::Absent;
    QPointer<QWindow> _window {};
    QPropertyAnimation _animation;
    QuickTerminalFrames _frames {};
    QuickTerminalMotion _motionKind = QuickTerminalMotion::Slide;
    std::string _activationToken {};
};

} // namespace contour::window
