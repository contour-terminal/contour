// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp`: the command line around the MCP server, and the stdio loop that feeds it.
#pragma once

#include <contour/cli/ControlVerbs.hpp>
#include <contour/cli/McpServer.hpp>

#include <core/async/StopToken.hpp>
#include <core/cli/CLI.hpp>

#include <expected>
#include <iosfwd>
#include <string>
#include <string_view>

namespace contour::cli
{

/// The options of `contour mcp`, beside the connection options every control verb takes.
/// @return Six options: `--allow-all-tabs`, `--read-only`, `--daemon`, `--token-file`, `--tls-ca` and
///         `--daemon-fingerprint`.
[[nodiscard]] core::cli::OptionList mcpOptions();

/// Builds the server's options from the command line and the environment.
/// @param flags The parsed command line.
/// @param env Where `$CONTOUR_PANE` is read.
/// @param serverVersion Reported as `serverInfo.version`.
/// @return The options, or why the command line is unusable.
[[nodiscard]] std::expected<McpOptions, std::string> makeMcpOptions(core::cli::FlagStore const& flags,
                                                                    core::Environment const& env,
                                                                    std::string_view serverVersion);

/// Serves MCP over newline-delimited JSON.
///
/// Each non-blank line of @p in is one request; each gets one reply line on @p out, flushed, and a
/// notification gets none. Nothing else is ever written to @p out: a stdio client takes every byte there
/// for a message.
/// @param options How the server is run.
/// @param channel Where the control calls go.
/// @param in The requests.
/// @param out The replies.
/// @param stop Ends the loop between two requests.
/// @return Ok at end of input, Interrupted once @p stop was requested.
[[nodiscard]] int runMcpServer(McpOptions options,
                               ControlChannel& channel,
                               std::istream& in,
                               std::ostream& out,
                               core::async::StopToken const& stop);

/// Runs `contour mcp`: reads the flags, dials the daemon and serves on `io.in` and `io.out`.
/// @param flags The parsed command line.
/// @param io Environment, files and streams; `io.err` carries the diagnostics.
/// @param serverVersion Reported as `serverInfo.version`.
/// @return The process exit status.
[[nodiscard]] int runMcpCommand(core::cli::FlagStore const& flags,
                                ControlIo const& io,
                                std::string_view serverVersion);

} // namespace contour::cli
