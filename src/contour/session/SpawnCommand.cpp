// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SpawnCommand.hpp>

using std::string;

namespace contour::session
{

SpawnTerminalCommand buildSpawnTerminalCommand(string const& programPath,
                                               string const& configPath,
                                               string const& profileName,
                                               string const& workingDirectory)
{
    SpawnTerminalCommand command;
    command.program = QString::fromUtf8(programPath.c_str());

    if (!configPath.empty())
        command.arguments << "config" << QString::fromStdString(configPath);

    if (!profileName.empty())
        command.arguments << "profile" << QString::fromStdString(profileName);

    if (!workingDirectory.empty())
        command.arguments << "working-directory" << QString::fromStdString(workingDirectory);

    return command;
}

SpawnTerminalCommand buildAttachHostCommand(string const& programPath,
                                            string const& configPath,
                                            string const& hostName)
{
    auto command = SpawnTerminalCommand {};
    command.program = QString::fromUtf8(programPath.c_str());
    command.arguments << "client" << "--host" << QString::fromStdString(hostName);
    if (!configPath.empty())
        command.arguments << "--config" << QString::fromStdString(configPath);
    return command;
}

} // namespace contour::session
