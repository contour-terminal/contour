// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/WindowGeometry.h>

#include <crispy/utils.h>

#include <QtCore/QFileSystemWatcher>
#include <QtCore/QObject>

#include <optional>
#include <string_view>

class QScreen;
class QWindow;

namespace contour::display
{

/// Parses a forced-font-DPI value out of a kcmfonts-style `key=value` document.
///
/// Pure (no filesystem, no Qt) so the parse rule is unit-testable: finds the first `@p key=<n>` line and
/// returns @c n only when it is a valid integer >= 96 (values below 96 mean "no forcing" — the
/// resolveContentScale contract ignores them, so they are reported as absent). @c std::nullopt when the
/// key is missing or its value is not a forcing DPI.
/// @param contents The whole config file's text.
/// @param key      The setting key to look up (forceFontDPI or forceFontDPIWayland).
/// @return The forced DPI (>= 96) or std::nullopt.
[[nodiscard]] inline std::optional<double> parseForcedFontDpi(std::string_view contents, std::string_view key)
{
    for (auto const line: crispy::split(contents, '\n'))
    {
        auto const fields = crispy::split(line, '=');
        if (fields.size() == 2 && fields[0] == key)
        {
            auto const forcedDPI = static_cast<double>(crispy::to_integer(fields[1]).value_or(0));
            if (forcedDPI >= 96.0)
                return forcedDPI;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

/// App-wide provider of the platform's forced-font-DPI override (KDE's kcmfonts setting).
///
/// Exactly ONE instance exists (owned by ContourGuiApp) and is injected into every consumer: the value is
/// parsed once and cached, refreshed only by the file watcher — unlike the historic implementation, which
/// re-read and re-parsed the file on every contentScale() call (i.e. per frame and several times per
/// resize step) and ran one watcher per display.
///
/// Platform-aware: KDE Wayland sessions write `forceFontDPIWayland`, X11 sessions `forceFontDPI`; the
/// matching key is chosen from QGuiApplication::platformName(). (The historic code read only the X11 key,
/// honoring a stale X11-only setting in Wayland sessions.) On macOS/Windows the provider is inert.
class ForcedFontDpiProvider: public QObject
{
    Q_OBJECT

  public:
    /// Creates the provider: parses the platform's font-config file once and starts watching it.
    /// @param parent Standard QObject ownership parent.
    explicit ForcedFontDpiProvider(QObject* parent = nullptr);

    /// @return The forced font DPI (e.g. 144.0), or std::nullopt when none is configured.
    [[nodiscard]] std::optional<double> value() const noexcept { return _cached; }

  signals:
    /// Emitted when the watched configuration file changes the effective forced DPI.
    void changed();

  private:
    void reload();
    void rewatch();

    QFileSystemWatcher _watcher;
    std::optional<double> _cached;
};

/// Content scale for a not-yet-mapped window targeting @p screen (the pre-show best guess).
///
/// Shares geometry::resolveContentScale with the runtime path below, so the pre-show cell-size guess and
/// the renderer can only diverge when the INPUTS change (window lands on another screen, fractional scale
/// arrives late) — never by implementation drift.
/// @param screen   The target screen, or nullptr when unknown.
/// @param provider The app's forced-DPI provider, or nullptr when not (yet) injected.
/// @return The resolved content scale (device pixels per logical pixel).
[[nodiscard]] double contentScaleForScreen(QScreen const* screen,
                                           ForcedFontDpiProvider const* provider) noexcept;

/// Content scale for a (possibly mapped) window. QWindow::devicePixelRatio() already falls back to the
/// associated screen's ratio pre-map, so a separate screen guess is not needed here.
/// @param window   The window, or nullptr when none exists yet.
/// @param provider The app's forced-DPI provider, or nullptr when not (yet) injected.
/// @return The resolved content scale (device pixels per logical pixel).
[[nodiscard]] double contentScaleForWindow(QWindow const* window,
                                           ForcedFontDpiProvider const* provider) noexcept;

} // namespace contour::display
