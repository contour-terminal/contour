// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsWindowHost.hpp>

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

int SettingsWindowHost::windowFlags() const noexcept
{
    // QML's `flags` takes the window flags as an int; every flag Qt defines fits in its 31 value bits.
    return static_cast<int>(settingsWindowFlags(_conventions.buttons).toInt());
}

std::expected<void, SettingsWindowError> SettingsWindowHost::show(QScreen* near)
{
    _controller.refresh();

    auto const existed = _window != nullptr;
    if (!existed)
    {
        auto built = _factory(*this);
        if (!built)
            return std::unexpected { built.error() };
        _window = std::move(*built);
    }

    auto& window = *_window;
    if (!window.isVisible())
        place(near, existed);
    if (window.windowStates().testFlag(Qt::WindowMinimized))
        window.setWindowStates(window.windowStates() & ~Qt::WindowMinimized);
    window.show();
    window.raise();
    window.requestActivate();
    return {};
}

void SettingsWindowHost::place(QScreen* near, bool existed)
{
    auto& window = *_window;
    auto* screen = near != nullptr ? near : QGuiApplication::primaryScreen();
    if (screen == nullptr)
        return;

    // Realize the still-hidden window now, as WindowController::restoreWindowPosition does, so its actual
    // native frame participates in the clamp: an unrealized window reports no frame, and a client placed at
    // the top of the work area would put its title bar out of reach.
    if (window.handle() == nullptr)
    {
        window.setScreen(screen);
        window.create();
    }

    auto const screens =
        QGuiApplication::screens()
        | std::views::transform([](QScreen const* each) { return each->availableGeometry(); })
        | std::ranges::to<std::vector>();
    auto const geometry = settingsWindowGeometry({
        .preferred = window.property("preferredSize").toSizeF().toSize(),
        .minimum = window.minimumSize(),
        .previous = existed ? std::optional { window.geometry() } : std::nullopt,
        .anchor = screen->availableGeometry(),
        .screens = screens,
        .frame = window.frameMargins(),
    });

    // Wayland ignores a client's position; only the size is ours to choose there.
    if (QGuiApplication::platformName() == QStringLiteral("wayland"))
        window.resize(geometry.size());
    else
        window.setGeometry(geometry);
}

void SettingsWindowHost::closeSettings()
{
    if (_window)
        _window->hide();
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
        errorLog()("The Settings window did not build a window: {}", component.errorString().toStdString());
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
