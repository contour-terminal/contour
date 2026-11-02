// SPDX-License-Identifier: Apache-2.0

#include <contour/cli/ExitStatus.hpp>
#include <contour/cli/McpCommand.hpp>

#include <core/Environment.hpp>

#include <istream>
#include <ostream>
#include <utility>

using namespace std::string_literals;

namespace contour::cli
{

core::cli::OptionList mcpOptions()
{
    namespace CLI = core::cli;
    return CLI::OptionList {
        CLI::Option { "allow-all-tabs",
                      CLI::Value { false },
                      "Lets the agent reach the panes of every tab, not just its own." },
        CLI::Option { "read-only",
                      CLI::Value { false },
                      "Serves only the tools that neither type into panes nor change the workspace." },
        CLI::Option { "daemon",
                      CLI::Value { ""s },
                      "The daemon to talk to: a socket path, HOST:PORT, or a host of hosts: by its name "
                      "or an ssh:// URL. Defaults to $CONTOUR_DAEMON, then the default socket.",
                      "PATH|HOST:PORT|NAME|URL" },
        CLI::Option { "token-file",
                      CLI::Value { ""s },
                      "Reads the token a HOST:PORT daemon expects from FILE.",
                      "FILE" },
        CLI::Option { "tls-ca",
                      CLI::Value { ""s },
                      "The CA certificate (PEM) a HOST:PORT daemon is verified against.",
                      "FILE" },
    };
}

std::expected<McpOptions, std::string> makeMcpOptions(core::cli::FlagStore const& flags,
                                                      core::Environment const& env,
                                                      std::string_view serverVersion)
{
    auto options = McpOptions {
        .scope = flags.boolean("contour.mcp.allow-all-tabs") ? TabScope::AllTabs : TabScope::OwnTab,
        .access = flags.boolean("contour.mcp.read-only") ? McpAccess::ReadOnly : McpAccess::Full,
        .callerPane = env.get("CONTOUR_PANE").value_or(""s),
        .serverVersion = std::string { serverVersion },
    };
    if (options.callerPane.empty() && options.scope == TabScope::OwnTab)
        return std::unexpected(
            "$CONTOUR_PANE is not set, so there is no own tab to scope the agent to; run it inside a "
            "Contour pane, or pass --allow-all-tabs"s);
    return options;
}

int runMcpServer(McpOptions options,
                 ControlChannel& channel,
                 std::istream& in,
                 std::ostream& out,
                 core::async::StopToken const& stop)
{
    auto server = McpServer { channel, std::move(options) };
    auto line = std::string {};
    while (!stop.stop_requested() && std::getline(in, line))
    {
        if (line.ends_with('\r'))
            line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos)
            continue;
        if (auto const reply = server.handleLine(line))
            out << *reply << '\n' << std::flush;
    }
    return exitCode(stop.stop_requested() ? ExitStatus::Interrupted : ExitStatus::Ok);
}

int runMcpCommand(core::cli::FlagStore const& flags, ControlIo const& io, std::string_view serverVersion)
{
    auto const options = makeMcpOptions(flags, io.env, serverVersion);
    if (!options)
    {
        io.err << "contour mcp: " << options.error() << '\n';
        return exitCode(ExitStatus::BadUsage);
    }
    auto channel = openControlChannel(flags, "mcp", io);
    if (!channel)
        return exitCode(channel.error());
    return runMcpServer(*options, **channel, io.in, io.out, io.stop);
}

} // namespace contour::cli
