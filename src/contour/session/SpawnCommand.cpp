// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SpawnCommand.hpp>

#include <vtbackend/FileUrl.hpp>

#include <QtCore/QUrl>

using std::string;

namespace contour::session
{

SpawnTerminalCommand buildSpawnTerminalCommand(string const& programPath,
                                               string const& configPath,
                                               string const& profileName,
                                               string const& cwdUrl,
                                               string const& localHost)
{
    // OSC 7 reports the cwd as file://HOST/PATH. Only a directory on THIS machine exists for the new
    // process, whichever way the host was spelled (@see vtbackend::isLocalHost).
    auto const url = QUrl(QString::fromStdString(cwdUrl));
    auto const wd = vtbackend::isLocalHost(url.host().toStdString(), localHost) ? url.path() : QString {};

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
