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

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace contour::window
{

namespace
{
    /// What @p state remembers of the Settings window; none when it cannot be read, which is logged.
    [[nodiscard]] std::optional<config::SettingsWindowState> rememberedIn(config::GuiStateStore const& state)
    {
        auto loaded = state.load();
        if (!loaded)
        {
            errorLog()("Could not read the GUI state: {}", config::describe(loaded.error()));
            return std::nullopt;
        }
        return std::move(loaded->settingsWindow);
    }

    /// The screen named @p name; null when there is none by that name.
    [[nodiscard]] QScreen* screenNamed(std::string const& name)
    {
        if (name.empty())
            return nullptr;
        auto const screens = QGuiApplication::screens();
        auto const found = std::ranges::find_if(
            screens, [&name](QScreen const* screen) { return screen->name().toStdString() == name; });
        return found != screens.end() ? *found : nullptr;
    }

    /// @p kinds as the config::WindowControlKind values QML reads.
    [[nodiscard]] QVariantList kindsOf(std::vector<config::WindowControlKind> const& kinds)
    {
        auto values = QVariantList {};
        for (auto const kind: kinds)
            values.push_back(static_cast<int>(kind));
        return values;
    }
} // namespace

SettingsWindowHost::SettingsWindowHost(SettingsController& controller,
                                       SettingsWindowConventions conventions,
                                       SettingsWindowFactory factory,
                                       SettingsWindowDressing dressing,
                                       config::GuiStateStore& state,
                                       SettingsPreview::SessionFactory previewFactory,
                                       std::function<void()> editConfiguration,
                                       std::function<void(std::string const&)> openInNewTab,
                                       QObject* parent):
    QObject(parent),
    _controller { controller },
    _conventions { conventions },
    _factory { std::move(factory) },
    _dressing { std::move(dressing) },
    _state { state },
    _editConfiguration { std::move(editConfiguration) },
    _openInNewTab { std::move(openInNewTab) },
    _preview { previewFactory ? std::make_unique<SettingsPreview>(controller, std::move(previewFactory))
                              : nullptr },
    _remembered { rememberedIn(state) },
    _lastPage { _remembered ? QString::fromStdString(_remembered->page) : QString {} },
    _windowControls { currentWindowControls() }
{
}

config::WindowButtonLayout SettingsWindowHost::currentWindowControls() const
{
    if (!frameless() || !_dressing.placement)
        return {};
    return settingsWindowControls(_dressing.placement(), _conventions.buttons);
}

void SettingsWindowHost::refreshWindowControls()
{
    auto controls = currentWindowControls();
    if (controls == _windowControls)
        return;
    _windowControls = std::move(controls);
    emit windowControlsChanged();
}

QVariantList SettingsWindowHost::leadingControls() const
{
    return kindsOf(_windowControls.leading);
}

QVariantList SettingsWindowHost::trailingControls() const
{
    return kindsOf(_windowControls.trailing);
}

void SettingsWindowHost::editConfigurationFile()
{
    if (_editConfiguration)
        _editConfiguration();
}

void SettingsWindowHost::openProfileInNewTab(QString const& name)
{
    if (_openInNewTab)
        _openInNewTab(name.toStdString());
}

void SettingsWindowHost::setLastPage(QString const& page)
{
    if (_lastPage == page)
        return;
    _lastPage = page;
    emit lastPageChanged();
}

void SettingsWindowHost::requestPage(QString const& page)
{
    setLastPage(page);
    emit pageRequested(page);
}

SettingsWindowHost::~SettingsWindowHost()
{
    // A window destroyed with its host is no close: it neither writes nor remembers, and its last signals
    // must not reach a host half torn down.
    if (_window)
    {
        _window->disconnect(this);
        _window->removeEventFilter(this);
    }
}

bool SettingsWindowHost::eventFilter(QObject* watched, QEvent* event)
{
    // A palette change follows every flip of the chrome's light or dark: a reload, an OSC 11, the OS's own.
    if (_window && watched == _window.get()
        && (event->type() == QEvent::Expose || event->type() == QEvent::ApplicationPaletteChange))
        dress();
    return QObject::eventFilter(watched, event);
}

void SettingsWindowHost::dress()
{
    auto& window = *_window;
    // On every expose, as WindowController seeds its frame: the frame skips what it already applied, and
    // re-tints its title bar for a scheme that changed.
    _dressing.frame.apply(window, _dressing.decoration);

    // Applied only to a platform window that exists: the material leaves one without a handle alone, and
    // recording such a request as applied would skip the first real apply.
    if (window.handle() == nullptr)
        return;
    auto const request = _backdropHealth == platform::BandMaterialHealth::Working
                             ? settingsBackdropFor(_dressing.inputs())
                             : std::nullopt;
    if (request == _appliedBackdrop)
        return;
    // A request withdrawn (Reduced transparency, a contrast theme) takes the material back off.
    auto const applied = _dressing.material.apply(
        window, request.value_or(platform::TitleBandRequest { .kind = config::BackdropKind::Main }));
    if (!applied)
    {
        errorLog()("The Settings window's material failed ({}); it paints its own background.",
                   platform::titleBandMaterialErrorName(applied.error()));
        _backdropHealth = platform::BandMaterialHealth::Failed;
        _appliedBackdrop.reset();
    }
    else
        _appliedBackdrop = request;
}

QString SettingsWindowHost::title() const
{
    return QString::fromUtf8(_conventions.title.data(), static_cast<qsizetype>(_conventions.title.size()));
}

Qt::WindowFlags SettingsWindowHost::windowFlags() const noexcept
{
    // Its own frame draws its own buttons: the OS is asked for none.
    if (frameless())
        return Qt::Window | Qt::FramelessWindowHint;
    return settingsWindowFlags(_conventions.buttons);
}

std::expected<void, SettingsWindowError> SettingsWindowHost::show(QScreen* anchor)
{
    // Where a window built by an earlier show was left; a window this show builds has the place the run
    // before left it at, if any.
    auto previous = std::optional<QRect> {};
    auto restoreMaximized = false;
    if (_window)
        previous = _window->geometry();
    else
    {
        auto built = _factory(*this);
        if (!built)
            return std::unexpected { built.error() };
        _window = std::move(*built);
        _window->installEventFilter(this);
        // However the window goes -- the page's Close, the shortcut, the title bar's button -- what is staged
        // is written then, not 300 ms later, when the app may be gone, and where it was is remembered.
        connect(_window.get(), &QWindow::visibleChanged, this, [this](bool visible) {
            if (!visible)
                onHidden();
        });
        for (auto const signal:
             { &QWindow::xChanged, &QWindow::yChanged, &QWindow::widthChanged, &QWindow::heightChanged })
            connect(_window.get(), signal, this, &SettingsWindowHost::trackNormalGeometry);
        if (auto const remembered = std::exchange(_remembered, std::nullopt))
        {
            previous = QRect(remembered->x, remembered->y, remembered->width, remembered->height);
            if (auto* const screen = screenNamed(remembered->screen))
                anchor = screen;
            restoreMaximized = remembered->show == config::WindowShowState::Maximized;
        }
    }

    auto& window = *_window;
    if (!window.isVisible())
    {
        // Only a window coming on screen reads the configuration afresh and is placed: one already open
        // may be mid-edit, and a refresh would rebuild the rows under the field being typed into (a
        // reload refreshes the controller by itself).
        _controller.refresh();
        // A maximised or full-screen window keeps its geometry, and with it the screen it closed on.
        if (settingsWindowReopenFor(window.windowStates()) == SettingsWindowReopen::Place)
            place(anchor, previous);
        // Placed at its normal frame first, so that leaving the maximised state returns to it.
        if (restoreMaximized)
            window.setWindowStates(Qt::WindowMaximized);
        // Not show(), which is showNormal() on the desktop: a window closed maximised reopens maximised.
        window.setVisible(true);
    }
    // Dressed now, with the platform window shown, rather than only at its first expose, which the
    // platform may deliver later.
    dress();
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

    // A loop, not views::transform | ranges::to: clang 22 on libstdc++ 14 rejects that pipeline here.
    auto const screens = [] {
        auto const all = QGuiApplication::screens();
        auto geometries = std::vector<QRect> {};
        geometries.reserve(static_cast<size_t>(all.size()));
        for (auto const* each: all)
            geometries.push_back(each->availableGeometry());
        return geometries;
    }();
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

void SettingsWindowHost::trackNormalGeometry()
{
    if (_window && (_window->windowStates() & (Qt::WindowMaximized | Qt::WindowFullScreen)) == 0)
        _normalGeometry = _window->geometry();
}

void SettingsWindowHost::onHidden()
{
    _controller.flushPendingChanges();
    if (!_window)
        return;

    auto& window = *_window;
    auto const maximized = (window.windowStates() & Qt::WindowMaximized) != 0;
    auto const frame = maximized ? _normalGeometry.value_or(window.geometry()) : window.geometry();
    auto state = _state.load().value_or(config::GuiState {});
    state.settingsWindow = config::SettingsWindowState {
        .x = frame.x(),
        .y = frame.y(),
        .width = frame.width(),
        .height = frame.height(),
        .screen = window.screen() != nullptr ? window.screen()->name().toStdString() : std::string {},
        .show = maximized ? config::WindowShowState::Maximized : config::WindowShowState::Normal,
        .page = _lastPage.toStdString(),
    };
    if (auto const saved = _state.save(state); !saved)
        errorLog()("Could not remember the Settings window: {}", config::describe(saved.error()));
}

void SettingsWindowHost::closeSettings()
{
    _controller.flushPendingChanges();
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
