// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/PortalExternalLauncher.hpp>

    #include <QtCore/QVariantMap>

    #include <utility>

namespace contour::platform
{

namespace
{
    constexpr auto OpenUriMethod = QLatin1StringView("OpenURI");

    // What the desktop is asked to run when the portal refuses. The freedesktop-standard opener,
    // present wherever a portal is; deliberately NOT QDesktopServices::openUrl, whose portal path
    // is the blocking call this whole class exists to avoid. @see the class documentation.
    constexpr auto FallbackOpener = QLatin1StringView("xdg-open");

} // namespace

QVariantList buildOpenUriArguments(QUrl const& url)
{
    return QVariantList {
        QString {},                       // parent_window: @see the declaration.
        url.toString(QUrl::FullyEncoded), // uri
        QVariantMap {},                   // options: no version-1 key applies to a plain open.
    };
}

PortalExternalLauncher::PortalExternalLauncher(PortalCaller caller, QObject* parent):
    QObject(parent), _call { std::move(caller) }
{
}

std::expected<void, LaunchError> PortalExternalLauncher::openUrl(QUrl const& url)
{
    if (url.isEmpty() || !url.isValid())
        return std::unexpected(LaunchError::InvalidUrl);

    if (!_call)
        return std::unexpected(LaunchError::DispatchFailed);

    _call(this, OpenUriMethod, buildOpenUriArguments(url), [this, url](CallOutcome outcome) {
        if (outcome == CallOutcome::Accepted)
            return;

        auto const urlText = url.toString();

        // The portal answered, and refused -- so it is responsive, and xdg-open will not hang either.
        // Reported rather than returned: by now the click has visibly finished and there is nothing
        // left for the caller to decide. @see ExternalLauncher::openUrl on what a success means.
        launcherLog()("Portal declined to open \"{}\"; falling back to {}.",
                      urlText.toStdString(),
                      asStringView(FallbackOpener));

        if (!runDetached(QString(FallbackOpener), QStringList { urlText }))
            launcherLog()("Could not open \"{}\": neither the portal nor {} would take it.",
                          urlText.toStdString(),
                          asStringView(FallbackOpener));
    });

    return {};
}

bool PortalExternalLauncher::runDetached(QString const& program, QStringList const& arguments)
{
    return _processes.runDetached(program, arguments);
}

int PortalExternalLauncher::execute(QString const& program, QStringList const& arguments)
{
    return _processes.execute(program, arguments);
}

} // namespace contour::platform

#endif // defined(__linux__)
