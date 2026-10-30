// SPDX-License-Identifier: Apache-2.0
#include <contour/window/QuickTerminalController.hpp>

#include <QtCore/QEasingCurve>
#include <QtGui/QScreen>

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

namespace contour::window
{

namespace
{
    /// The animated property of each motion, in QuickTerminalMotion's order.
    constexpr auto MotionProperties = std::array<char const*, 2> { "y", "opacity" };

    /// @return @p rect as Qt's.
    [[nodiscard]] QRect toQRect(ScreenRect const& rect) noexcept
    {
        return QRect { rect.x, rect.y, rect.width, rect.height };
    }

    /// @return Qt's @p rect as a ScreenRect.
    [[nodiscard]] ScreenRect toScreenRect(QRect const& rect) noexcept
    {
        return ScreenRect { .x = rect.x(), .y = rect.y(), .width = rect.width(), .height = rect.height() };
    }
} // namespace

QRegion roundedBottomMask(int width, int height, int radius)
{
    auto const r = std::clamp(radius, 0, std::min(width / 2, height));
    if (r == 0)
        return QRegion { 0, 0, width, height };
    auto const diameter = 2 * r;
    // The body down to where the corners start, the strip between the corners, and the two corner discs
    // (whose upper halves lie inside the body).
    return QRegion { 0, 0, width, height - r } | QRegion { r, height - r, width - diameter, r }
           | QRegion { QRect { 0, height - diameter, diameter, diameter }, QRegion::Ellipse }
           | QRegion { QRect { width - diameter, height - diameter, diameter, diameter }, QRegion::Ellipse };
}

QuickTerminalController::QuickTerminalController(QuickTerminalHost host,
                                                 config::QuickTerminalConfig config,
                                                 config::MotionTokens motion,
                                                 QObject* parent):
    QObject { parent }, _host { std::move(host) }, _config { config }, _motion { motion }, _animation { this }
{
    _animation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&_animation, &QAbstractAnimation::finished, this, [this] { onAnimationFinished(); });
    applyHotkey();
}

void QuickTerminalController::reconfigure(config::QuickTerminalConfig config, config::MotionTokens motion)
{
    _config = config;
    _motion = motion;
    applyHotkey();
}

void QuickTerminalController::press(std::string activationToken)
{
    _activationToken = std::move(activationToken);
    dispatch(QuickTerminalEvent::Hotkey);
}

void QuickTerminalController::applyHotkey()
{
    if (_config.hotkey && _config.hotkey == _registered)
        return;
    _registered.reset();
    if (!_config.hotkey)
    {
        // Also after a refusal, when nothing is recorded but the hotkey may still hold an earlier chord.
        _host.hotkey.unregisterHotkey();
        return;
    }
    auto const chord = *_config.hotkey;
    if (_host.isMenuOwned && _host.isMenuOwned(chord))
    {
        // Refused before the OS is asked; the chord a previous registration holds is released too.
        _host.hotkey.unregisterHotkey();
        _host.onHotkeyError(platform::HotkeyError::MenuOwned);
        return;
    }
    // Recorded while the answer is out, so an unchanged chord is not sent twice; a refusal takes it back
    // below, so that the next reconfiguration retries it.
    _registered = chord;
    // The settled outcome arrives through the result callback, an immediate refusal included; the returned
    // value repeats it.
    std::ignore = _host.hotkey.registerHotkey(
        chord,
        [this](std::string activationToken) { press(std::move(activationToken)); },
        [this, chord](platform::HotkeyResult result) {
            // An answer for a chord that is no longer the configured one says nothing of this one.
            if (_registered != chord)
                return;
            if (result)
            {
                if (_host.onHotkeyBound)
                    _host.onHotkeyBound();
                return;
            }
            _registered.reset();
            _host.onHotkeyError(result.error());
        });
}

QuickTerminalFocus QuickTerminalController::focus() const
{
    return _window && _window->isActive() ? QuickTerminalFocus::Focused : QuickTerminalFocus::Unfocused;
}

void QuickTerminalController::dispatch(QuickTerminalEvent event)
{
    auto const step = quickTerminalStep(_state, event, focus(), _config.focusLoss);
    if (!step)
        return;
    _state = step->to;
    perform(step->effect);
}

void QuickTerminalController::perform(QuickTerminalEffect effect)
{
    switch (effect)
    {
        case QuickTerminalEffect::None: break;
        case QuickTerminalEffect::CreateAndShow: createAndShow(); break;
        case QuickTerminalEffect::Show: showWindow(); break;
        case QuickTerminalEffect::Hide: startSlide(QAbstractAnimation::Backward); break;
        case QuickTerminalEffect::Focus:
            if (_window)
                _host.panel.present(*_window, _activationToken);
            break;
        case QuickTerminalEffect::Reverse:
            // From where it is: the running animation turns round, and its end is the other state's.
            _animation.setDirection(_animation.direction() == QAbstractAnimation::Forward
                                        ? QAbstractAnimation::Backward
                                        : QAbstractAnimation::Forward);
            break;
        case QuickTerminalEffect::Destroy:
            _animation.stop();
            _animation.setTargetObject(nullptr);
            _window.clear();
            noteOffScreen();
            break;
    }
}

void QuickTerminalController::createAndShow()
{
    auto* const window = _host.createWindow ? _host.createWindow() : nullptr;
    if (window == nullptr)
    {
        _state = QuickTerminalState::Absent;
        return;
    }
    _window = window;
    // The window is gone when its last tab closes (its QML root is deleted), and loses or gains the focus
    // as the user clicks away and back.
    connect(window, &QObject::destroyed, this, [this] { dispatch(QuickTerminalEvent::LastTabClosed); });
    connect(window, &QWindow::activeChanged, this, [this] {
        if (_window)
            dispatch(_window->isActive() ? QuickTerminalEvent::FocusGained : QuickTerminalEvent::FocusLost);
    });
    connect(window, &QWindow::screenChanged, this, &QuickTerminalController::replaceOnScreenChange);
    _host.panel.adopt(*window);
    showWindow();
}

void QuickTerminalController::showWindow()
{
    if (!_window)
        return;
    _frames = quickTerminalFrames(_host.screenUnderPointer());
    auto const others = _host.otherScreens ? _host.otherScreens() : std::vector<ScreenRect> {};
    auto const animation =
        quickTerminalAnimation(_motion, _host.placement, quickTerminalSlideClearance(_frames, others));
    _motionKind = animation.motion;
    _animation.setDuration(static_cast<int>(animation.duration.count()));
    _animation.setTargetObject(_window.data());
    _animation.setPropertyName(MotionProperties[std::to_underlying(_motionKind)]);
    switch (_motionKind)
    {
        case QuickTerminalMotion::Slide:
            _window->setGeometry(toQRect(_frames.hidden));
            _animation.setStartValue(_frames.hidden.y);
            _animation.setEndValue(_frames.shown.y);
            break;
        case QuickTerminalMotion::Fade:
            _window->setGeometry(toQRect(_frames.shown));
            _window->setOpacity(0.0);
            _animation.setStartValue(0.0);
            _animation.setEndValue(1.0);
            break;
    }
    _window->setMask(roundedBottomMask(
        _frames.shown.width, _frames.shown.height, _host.cornerRadius ? _host.cornerRadius() : 0));
    _host.panel.present(*_window, _activationToken);
    startSlide(QAbstractAnimation::Forward);
}

void QuickTerminalController::startSlide(QAbstractAnimation::Direction direction)
{
    if (!_window)
        return;
    _animation.setDirection(direction);
    _animation.start();
}

void QuickTerminalController::onAnimationFinished()
{
    dispatch(QuickTerminalEvent::AnimationDone);
    if (_state == QuickTerminalState::Hidden && _window)
    {
        _host.panel.dismiss(*_window);
        noteOffScreen();
    }
}

void QuickTerminalController::noteOffScreen() const
{
    if (_host.onOffScreen)
        _host.onOffScreen();
}

void QuickTerminalController::replaceOnScreenChange(QScreen* screen)
{
    // A window that is on the screen follows a screen that went away to another one, which Qt moves it to,
    // and the frame is then laid out again there. Moves the controller made itself, while showing, land on
    // the screen it laid the frame out for, so they change nothing.
    if (screen == nullptr)
        return;
    switch (_state)
    {
        case QuickTerminalState::Absent:
        case QuickTerminalState::Hidden: return; // Laid out afresh at the next show.
        case QuickTerminalState::Showing:
        case QuickTerminalState::Shown:
        case QuickTerminalState::Hiding: break;
    }
    auto const available = toScreenRect(screen->availableGeometry());
    if (quickTerminalFrames(available).shown == _frames.shown)
        return;
    relayoutOn(available);
}

void QuickTerminalController::relayoutOn(ScreenRect available)
{
    if (!_window)
        return;
    // A slide that was under way ends where it was heading, on the new screen, instead of finishing a
    // travel measured against the old one.
    auto const moving = _state == QuickTerminalState::Showing || _state == QuickTerminalState::Hiding;
    if (moving)
        _animation.stop();
    _frames = quickTerminalFrames(available);
    _window->setMask(roundedBottomMask(
        _frames.shown.width, _frames.shown.height, _host.cornerRadius ? _host.cornerRadius() : 0));
    if (_state == QuickTerminalState::Hiding)
    {
        _window->setGeometry(toQRect(_frames.hidden));
        onAnimationFinished();
        return;
    }
    _window->setGeometry(toQRect(_frames.shown));
    _window->setOpacity(1.0);
    if (moving)
        onAnimationFinished();
}

} // namespace contour::window
