// SPDX-License-Identifier: Apache-2.0
#include <contour/QtExternalLauncher.h>

#include <QtCore/QProcess>
#include <QtGui/QDesktopServices>

namespace contour
{

bool QtExternalLauncher::openUrl(QUrl const& url)
{
    return QDesktopServices::openUrl(url);
}

bool QtExternalLauncher::runDetached(QString const& program, QStringList const& arguments)
{
    return QProcess::startDetached(program, arguments);
}

int QtExternalLauncher::execute(QString const& program, QStringList const& arguments)
{
    return QProcess::execute(program, arguments);
}

} // namespace contour
