// SPDX-License-Identifier: Apache-2.0
#include <contour/Config.h>
#include <contour/display/ContentScale.h>
#include <contour/helper.h>

#include <crispy/utils.h>

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QWindow>

#include <filesystem>
#include <string_view>

using namespace std::string_view_literals;

namespace contour::display
{

namespace
{
    /// The platform's font-configuration file carrying the forced-DPI setting, if the platform has one.
    std::optional<std::filesystem::path> fontConfigFilePath()
    {
#if !defined(__APPLE__) && !defined(_WIN32)
        if (auto kcmFontsFile = config::configHome("") / "kcmfonts"; std::filesystem::exists(kcmFontsFile))
            return kcmFontsFile;
#endif
        return std::nullopt;
    }

    /// The kcmfonts key for this session type: KDE writes `forceFontDPIWayland` in Wayland sessions and
    /// `forceFontDPI` in X11 sessions — two independent settings.
    std::string_view forcedDpiKey()
    {
        if (QGuiApplication::platformName() == QStringLiteral("wayland"))
            return "forceFontDPIWayland"sv;
        return "forceFontDPI"sv;
    }

    /// Reads the platform font-config file (if any) and extracts this session's forced-font-DPI. The
    /// pure line-parsing lives in the header's parseForcedFontDpi(contents, key) so it can be tested
    /// without a kcmfonts file on disk.
    std::optional<double> readForcedFontDpi()
    {
        auto const path = fontConfigFilePath();
        if (!path)
            return std::nullopt;
        return parseForcedFontDpi(crispy::readFileAsString(*path), forcedDpiKey());
    }
} // namespace

ForcedFontDpiProvider::ForcedFontDpiProvider(QObject* parent): QObject(parent)
{
    _cached = readForcedFontDpi();
    if (_cached)
        displayLog()("Forcing font DPI to {} (from platform font configuration).", *_cached);
    rewatch();
    connect(&_watcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        reload();
        // Editors replace-and-rename config files, which drops the watch; re-add the path.
        rewatch();
    });
}

void ForcedFontDpiProvider::reload()
{
    auto const fresh = readForcedFontDpi();
    if (fresh == _cached)
        return;
    displayLog()("Forced font DPI changed: {} -> {}.",
                 _cached ? std::format("{}", *_cached) : "none",
                 fresh ? std::format("{}", *fresh) : "none");
    _cached = fresh;
    emit changed();
}

void ForcedFontDpiProvider::rewatch()
{
    if (auto const path = fontConfigFilePath())
    {
        auto const qtPath = QString::fromStdString(path->string());
        if (!_watcher.files().contains(qtPath))
            _watcher.addPath(qtPath);
    }
}

double contentScaleForScreen(QScreen const* screen, ForcedFontDpiProvider const* provider) noexcept
{
    return geometry::resolveContentScale(provider != nullptr ? provider->value() : std::nullopt,
                                         std::nullopt,
                                         screen != nullptr ? std::optional(screen->devicePixelRatio())
                                                           : std::nullopt);
}

double contentScaleForWindow(QWindow const* window, ForcedFontDpiProvider const* provider) noexcept
{
    return geometry::resolveContentScale(provider != nullptr ? provider->value() : std::nullopt,
                                         window != nullptr ? std::optional(window->devicePixelRatio())
                                                           : std::nullopt,
                                         std::nullopt);
}

} // namespace contour::display
