// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/QtExternalLauncher.hpp>

#include <QtCore/QProcess>
#include <QtGui/QDesktopServices>

namespace contour::platform
{

std::expected<void, LaunchError> QtExternalLauncher::openUrl(QUrl const& url)
{
    if (url.isEmpty() || !url.isValid())
        return std::unexpected(LaunchError::InvalidUrl);

    // Qt reports one bool for two different failures -- no handler found, and a handler that could
    // not be started -- so the more specific of the two is not available to report here.
    if (!QDesktopServices::openUrl(url))
        return std::unexpected(LaunchError::NoHandler);

    return {};
}

bool QtExternalLauncher::runDetached(QString const& program, QStringList const& arguments)
{
    return QProcess::startDetached(program, arguments);
}

int QtExternalLauncher::execute(QString const& program, QStringList const& arguments)
{
    return QProcess::execute(program, arguments);
}

} // namespace contour::platform
