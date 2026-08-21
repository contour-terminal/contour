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

void PortalExternalLauncher::openWithFallback(QUrl const& url, std::string_view why)
{
    // Fully encoded, as buildOpenUriArguments() spells it: toString()'s default decodes percent
    // escapes back, which would hand xdg-open a "file:///tmp/some dir" that is not a URI at all.
    auto const urlText = url.toString(QUrl::FullyEncoded);

    // Reported rather than returned: by now the click has visibly finished and there is nothing left
    // for the caller to decide. @see ExternalLauncher::openUrl on what a success means.
    launcherLog()("Opening \"{}\" with {}: {}.", urlText.toStdString(), asStringView(FallbackOpener), why);

    if (auto const started = runDetached(QString(FallbackOpener), QStringList { urlText }); !started)
        launcherLog()("Could not open \"{}\": {} either -- {}.",
                      urlText.toStdString(),
                      asStringView(FallbackOpener),
                      describe(started.error()));
}

std::expected<void, LaunchError> PortalExternalLauncher::openUrl(QUrl const& url)
{
    if (!isOpenable(url))
        return std::unexpected(LaunchError::InvalidUrl);

    // OpenURI refuses a file: URI outright -- "Note that file:// URIs are explicitly not supported by
    // this method. To request opening local files, use OpenFile." -- so asking anyway would spend a
    // round trip to be told no and write a portal failure that is not one. OpenFile is the method
    // that takes them, but it takes an open FILE DESCRIPTOR rather than a URI, which this caller has
    // no way to pass; until it does, a local file goes straight to the opener the refusal path would
    // have reached in any case. Inside a Flatpak that opener is flatpak-xdg-utils' shim, which speaks
    // OpenFile with a descriptor of its own -- so this is the path that actually worked all along.
    if (url.isLocalFile())
    {
        openWithFallback(url, "the portal's OpenURI does not take local files");
        return {};
    }

    _call(this, OpenUriMethod, buildOpenUriArguments(url), [this, url](CallOutcome outcome) {
        if (outcome == CallOutcome::Accepted)
            return;

        // The portal answered, and refused -- so it is responsive, and xdg-open will not hang either.
        // Why it refused is not repeated here: PortalCall has already written that to gui.portal.
        openWithFallback(url, "the portal refused it");
    });

    return {};
}

std::expected<void, SpawnError> PortalExternalLauncher::runDetached(QString const& program,
                                                                    QStringList const& arguments)
{
    return _processes->runDetached(program, arguments);
}

std::expected<int, SpawnError> PortalExternalLauncher::execute(QString const& program,
                                                               QStringList const& arguments)
{
    return _processes->execute(program, arguments);
}

} // namespace contour::platform

#endif // defined(__linux__)
