// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__

    #include <contour/Logging.hpp>
    #include <contour/platform/PortalExternalLauncher.hpp>

    #include <crispy/Assert.hpp>

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

PortalExternalLauncher::PortalExternalLauncher(PortalCaller caller,
                                               std::unique_ptr<ExternalLauncher> processes,
                                               QObject* parent):
    QObject(parent), _call { std::move(caller) }, _processes { std::move(processes) }
{
    // Both collaborators are a construction-time requirement rather than a runtime error: a launcher
    // built without one is a wiring mistake, and there is no answer to give a user about it.
    Require(static_cast<bool>(_call));
    Require(_processes != nullptr);
}

std::expected<void, LaunchError> PortalExternalLauncher::openUrl(QUrl const& url)
{
    if (!isOpenable(url))
        return std::unexpected(LaunchError::InvalidUrl);

    _call(this, OpenUriMethod, buildOpenUriArguments(url), [this, url](CallOutcome outcome) {
        if (outcome == CallOutcome::Accepted)
            return;

        auto const urlText = url.toString();

        // The portal answered, and refused -- so it is responsive, and xdg-open will not hang either.
        // Reported rather than returned: by now the click has visibly finished and there is nothing
        // left for the caller to decide. @see ExternalLauncher::openUrl on what a success means.
        //
        // Why it refused is not repeated here: PortalCall has already written that to gui.portal.
        launcherLog()("Falling back to {} for \"{}\".", asStringView(FallbackOpener), urlText.toStdString());

        if (!runDetached(QString(FallbackOpener), QStringList { urlText }))
            launcherLog()("Could not open \"{}\": neither the portal nor {} would take it.",
                          urlText.toStdString(),
                          asStringView(FallbackOpener));
    });

    return {};
}

bool PortalExternalLauncher::runDetached(QString const& program, QStringList const& arguments)
{
    return _processes->runDetached(program, arguments);
}

int PortalExternalLauncher::execute(QString const& program, QStringList const& arguments)
{
    return _processes->execute(program, arguments);
}

} // namespace contour::platform

#endif // defined(__linux__)
