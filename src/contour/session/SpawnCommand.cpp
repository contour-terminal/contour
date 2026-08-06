// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SpawnCommand.hpp>

#include <QtCore/QUrl>
#include <QtNetwork/QHostInfo>

using std::string;

namespace contour::session
{

SpawnTerminalCommand buildSpawnTerminalCommand(string const& programPath,
                                               string const& configPath,
                                               string const& profileName,
                                               string const& cwdUrl)
{
    auto const wd = [&]() -> QString {
        auto const url = QUrl(QString::fromUtf8(cwdUrl.c_str()));

        if (url.host().isEmpty())
            return url.path();

        if (url.host() == QHostInfo::localHostName())
            return url.path();
        else
            return {};
    }();

    SpawnTerminalCommand command;
    command.program = QString::fromUtf8(programPath.c_str());

    if (!configPath.empty())
        command.arguments << "config" << QString::fromStdString(configPath);

    if (!profileName.empty())
        command.arguments << "profile" << QString::fromStdString(profileName);

    if (!wd.isEmpty())
        command.arguments << "working-directory" << wd;

    return command;
}

} // namespace contour::session
