// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/WindowActivation.hpp>

#include <core/log/LogStore.hpp>

#include <QtCore/QVariant>
#include <QtCore/QVariantMap>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>

#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace contour::window
{

SettingsWindowHost::SettingsWindowHost(SettingsController& controller,
                                       SettingsWindowConventions conventions,
                                       SettingsWindowFactory factory,
                                       QObject* parent):
    QObject(parent), _controller { controller }, _conventions { conventions }, _factory { std::move(factory) }
{
}

SettingsWindowHost::~SettingsWindowHost() = default;

QString SettingsWindowHost::title() const
{
    return QString::fromUtf8(_conventions.title.data(), static_cast<qsizetype>(_conventions.title.size()));
}

Qt::WindowFlags SettingsWindowHost::windowFlags() const noexcept
{
    return settingsWindowFlags(_conventions.buttons);
}

std::expected<void, SettingsWindowError> SettingsWindowHost::show(QScreen* anchor)
{
    // Where a window built by an earlier show was left; a window this show builds has no such place.
    auto previous = std::optional<QRect> {};
    if (_window)
        previous = _window->geometry();
    else
    {
        auto built = _factory(*this);
        if (!built)
            return std::unexpected { built.error() };
        _window = std::move(*built);
    }

    auto& window = *_window;
    if (!window.isVisible())
    {
        // Only a window coming on screen reads the configuration afresh and is placed: one already open
        // may be mid-edit, and a refresh would rebuild the rows under the field being typed into (a
        // reload refreshes the controller by itself).
        _controller.refresh();
        place(anchor, previous);
        // Not show(), which is showNormal() on the desktop: a window closed maximised reopens maximised.
        window.setVisible(true);
    }
    raiseAndActivate(window);
    return {};
}

void SettingsWindowHost::place(QScreen* anchor, std::optional<QRect> previous)
{
    auto& window = *_window;
    auto* screen = anchor != nullptr ? anchor : QGuiApplication::primaryScreen();
    if (screen == nullptr)
        return;

    // Wayland ignores a client's position; only the size is ours to choose there.
    auto const positionable = QGuiApplication::platformName() != QStringLiteral("wayland");

    // Realize the still-hidden window now, as WindowController::restoreWindowPosition does, so its actual
    // native frame participates in the clamp: an unrealized window reports no frame, and a client placed at
    // the top of the work area would put its title bar out of reach. setScreen() alone does not move a
    // window within a virtual desktop, so it is moved onto that screen first: creation then picks the
    // monitor, and with it the DPI and the frame, the window opens on.
    if (window.handle() == nullptr)
    {
        window.setScreen(screen);
        if (positionable)
            window.setPosition(screen->availableGeometry().topLeft());
        window.create();
    }

    auto const screens =
        QGuiApplication::screens()
        | std::views::transform([](QScreen const* each) { return each->availableGeometry(); })
        | std::ranges::to<std::vector>();
    auto const geometry = settingsWindowGeometry({
        .preferred = window.property("preferredSize").toSizeF().toSize(),
        .minimum = window.minimumSize(),
        .previous = previous,
        .anchor = screen->availableGeometry(),
        .screens = screens,
        .frame = window.frameMargins(),
    });

    if (positionable)
        window.setGeometry(geometry);
    else
        window.resize(geometry.size());
}

void SettingsWindowHost::closeSettings()
{
    if (_window)
        _window->close();
}

std::expected<std::unique_ptr<QQuickWindow>, SettingsWindowError> loadSettingsWindow(QQmlEngine& engine,
                                                                                     SettingsWindowHost& host)
{
    auto component = QQmlComponent(&engine);
    component.loadFromModule("Contour.Ui", "SettingsWindow");
    if (!component.isReady())
    {
        errorLog()("Could not load the Settings window: {}", component.errorString().toStdString());
        return std::unexpected { SettingsWindowError::LoadFailed };
    }

    auto object = std::unique_ptr<QObject>(component.createWithInitialProperties(
        QVariantMap { { QStringLiteral("host"), QVariant::fromValue(&host) } }));
    if (qobject_cast<QQuickWindow*>(object.get()) == nullptr)
    {
        // An object that was built has no component error to report; what it is says what went wrong.
        errorLog()("The Settings window did not build a window: {}",
                   object ? object->metaObject()->className() : component.errorString().toStdString());
        return std::unexpected { SettingsWindowError::LoadFailed };
    }
    return std::unique_ptr<QQuickWindow>(static_cast<QQuickWindow*>(object.release()));
}

Qt::WindowFlags settingsWindowFlags(SettingsWindowButtons buttons) noexcept
{
    switch (buttons)
    {
        case SettingsWindowButtons::CloseOnly:
            return Qt::Window | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint;
        case SettingsWindowButtons::All: return Qt::Window;
    }
    return Qt::Window;
}

} // namespace contour::window
