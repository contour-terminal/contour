// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/QtExternalLauncher.hpp>

#include <QtCore/QProcess>
#include <QtGui/QDesktopServices>

namespace contour::platform
{

std::expected<void, LaunchError> QtExternalLauncher::openUrl(QUrl const& url)
{
    if (!isOpenable(url))
        return std::unexpected(LaunchError::InvalidUrl);

    // Qt reports one bool for two different failures -- no handler found, and a handler that could
    // not be started -- so the more specific of the two is not available to report here.
    if (!QDesktopServices::openUrl(url))
        return std::unexpected(LaunchError::NoHandler);

    return {};
}

std::expected<void, SpawnError> QtExternalLauncher::runDetached(QString const& program,
                                                                QStringList const& arguments)
{
    if (!isReachableProgram(program))
        return std::unexpected(SpawnError::NotFound);

    if (!QProcess::startDetached(program, arguments))
        return std::unexpected(SpawnError::StartFailed);

    return {};
}

std::expected<int, SpawnError> QtExternalLauncher::execute(QString const& program,
                                                           QStringList const& arguments)
{
    if (!isReachableProgram(program))
        return std::unexpected(SpawnError::NotFound);

    // QProcess::execute() answers with two sentinels below zero and an exit code at or above it:
    // -2 could not be started, -1 started and crashed. Both are the absence of an exit code, not
    // an exit code, which is what separating the channels is for.
    auto const exitCode = QProcess::execute(program, arguments);
    if (exitCode == -2)
        return std::unexpected(SpawnError::StartFailed);
    if (exitCode == -1)
        return std::unexpected(SpawnError::Crashed);

    return exitCode;
}

} // namespace contour::platform
