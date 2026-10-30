// SPDX-License-Identifier: Apache-2.0
//
// The Linux factories of the appearance and accessibility sources: both read the XDG Settings portal.
// The BSDs keep the null sources of PlatformAppearance.cpp and AccessibilityPreferenceSource.cpp.
#ifdef __linux__

    #include <contour/config/DesktopButtonLayout.hpp>
    #include <contour/config/WindowControlStyle.hpp>
    #include <contour/platform/AccessibilityPreferenceSource.hpp>
    #include <contour/platform/DBusPortalSettingsChannel.hpp>
    #include <contour/platform/PlatformAppearance.hpp>
    #include <contour/platform/PortalAppearanceSources.hpp>

    #include <core/Environment.hpp>
    #include <core/platform/NativeFileSystem.hpp>
    #include <core/platform/UserPaths.hpp>

    #include <QtCore/QCoreApplication>

    #include <memory>
    #include <utility>

namespace contour::platform
{

namespace
{
    /// Whether a source may reach the session bus: only once the Qt application object exists (ruling
    /// R-i). The app builds its sources on first read, which a GUI run reaches after QGuiApplication; a
    /// read before that -- a headless verb asking for title-band tokens -- gets no source, so the app
    /// answers with the defaults and asks again on its next read rather than keeping a null source for good.
    [[nodiscard]] bool canReachTheBus() noexcept
    {
        return QCoreApplication::instance() != nullptr;
    }

    /// What the button layout is resolved from besides the portal: the desktop's files, read once now,
    /// and the session.
    ///
    /// The process's own filesystem and environment, rather than the app's injected ones: this factory is
    /// the composition point of AppearanceServices::appearance, whose signature carries only the change
    /// callback, and every test injects its own services instead of reaching it.
    [[nodiscard]] DesktopLayoutInputs desktopLayoutInputs()
    {
        auto const environment = core::LiveEnvironment {};
        auto const desktop = config::detectDesktopPlatform(environment);
        return DesktopLayoutInputs {
            .files =
                config::fileButtonLayoutSources({ .fileSystem = core::platform::NativeFileSystem::instance(),
                                                  .configHome = core::platform::configHome(environment),
                                                  .desktop = desktop }),
            .desktop = desktop,
        };
    }
} // namespace

std::unique_ptr<IPlatformAppearance> makePlatformAppearance(PlatformAppearanceChanged onChange)
{
    if (!canReachTheBus())
        return nullptr;
    return std::make_unique<PortalPlatformAppearance>(
        dbusPortalSettingsChannels(), desktopLayoutInputs(), std::move(onChange));
}

std::unique_ptr<IAccessibilityPreferenceSource> makeAccessibilityPreferenceSource(
    AccessibilityPreferencesChanged onChange)
{
    if (!canReachTheBus())
        return nullptr;
    return std::make_unique<PortalAccessibilityPreferenceSource>(dbusPortalSettingsChannels(),
                                                                 std::move(onChange));
}

} // namespace contour::platform

#endif // defined(__linux__)
