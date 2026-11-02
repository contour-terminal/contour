// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp`: the Model Context Protocol server. It speaks JSON-RPC 2.0 one message per line and
// serves the tools of McpTools.hpp by calling the control API through a ControlChannel.
#pragma once

#include <contour/cli/ControlVerbs.hpp>
#include <contour/cli/McpScope.hpp>
#include <contour/cli/McpTools.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contour::cli
{

/// The protocol revisions the server speaks, newest first.
inline constexpr auto McpProtocolVersions = std::array { std::string_view { "2025-06-18" },
                                                         std::string_view { "2025-03-26" },
                                                         std::string_view { "2024-11-05" } };

/// How the server is run.
struct McpOptions
{
    TabScope scope = TabScope::OwnTab;  ///< Which tabs the agent may reach.
    McpAccess access = McpAccess::Full; ///< Whether the agent may type or change the workspace.
    std::string callerPane;             ///< The full id of the pane the agent runs in; empty when unknown.
    std::string serverVersion = "0";    ///< Reported as `serverInfo.version`.
};

/// The MCP server's protocol engine: a request line in, a reply line out. It owns no I/O; the caller reads
/// lines and writes the replies.
///
/// Every pane the agent names is resolved against a fresh `workspace.list` and the daemon is sent the
/// resolved full id, never the agent's text, so a label cannot be re-resolved by the daemon across tabs.
/// A pane may move to another tab between that snapshot and the action; the window is accepted.
class McpServer
{
  public:
    /// @param channel Where the control calls go; must outlive the server.
    /// @param options How the server is run.
    McpServer(ControlChannel& channel, McpOptions options);

    /// @param line One JSON-RPC message.
    /// @return The reply line, or nothing for a notification.
    [[nodiscard]] std::optional<std::string> handleLine(std::string_view line);

  private:
    /// A decoded JSON-RPC message. An empty @ref id is a notification.
    struct Request
    {
        std::string id;     ///< The id as JSON text; empty when absent.
        std::string method; ///< The MCP method.
        std::string params; ///< The params as JSON text; empty when absent.
    };

    /// A JSON-RPC error.
    struct RpcError
    {
        int code = 0;        ///< The JSON-RPC error code.
        std::string message; ///< What went wrong.
    };

    /// What a tool produced.
    enum class ToolOutcome : uint8_t
    {
        Ok = 0,
        Error,
        BadArguments, ///< The arguments do not fit the tool's schema: a JSON-RPC `-32602`.
    };

    /// A tool's result: text for the agent, and whether it is an error.
    struct ToolResult
    {
        ToolOutcome outcome = ToolOutcome::Ok; ///< Whether the tool succeeded.
        std::string text;                      ///< The reply JSON, or the error message.
    };

    using RpcResult = std::expected<std::string, RpcError>;
    using Failure = std::string; ///< The message of a refused or failed step.

    [[nodiscard]] RpcResult dispatch(Request const& request);
    [[nodiscard]] RpcResult onInitialize(Request const& request);
    [[nodiscard]] RpcResult onInitialized(Request const& request);
    [[nodiscard]] RpcResult onPing(Request const& request);
    [[nodiscard]] RpcResult onToolsList(Request const& request);
    [[nodiscard]] RpcResult onToolsCall(Request const& request);

    [[nodiscard]] ToolResult runTool(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolWorkspaceList(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolDaemonInfo(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolPaneCapture(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolPaneSplit(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolPaneType(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolPaneRun(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolTabCreate(ToolRow const& row, std::string_view arguments);
    template <typename Request>
    [[nodiscard]] ToolResult toolOnPane(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolPaneClose(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolTabClose(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolSetLabel(ToolRow const& row, std::string_view arguments);
    [[nodiscard]] ToolResult toolBlockRead(ToolRow const& row, std::string_view arguments);

    /// @return The `workspace.list` snapshot, or why there is none (including a vanished caller pane).
    [[nodiscard]] std::expected<vtcontrol::ListReply, Failure> fetchWorkspace();
    /// @param spec What the agent wrote for a pane; empty names the caller's pane.
    /// @return The pane's full id under the scope rules, or why it cannot be used.
    [[nodiscard]] std::expected<std::string, Failure> resolveOwnedPane(std::string_view spec);
    /// Calls @p method with @p params and the caller's pane as context.
    [[nodiscard]] ToolResult callDaemon(std::string_view method, std::string const& params);
    /// Learns the connection's grants from `daemon.info`, once.
    void learnGrants();

    ControlChannel& _channel;
    McpOptions _options;
    /// Whether `daemon.info` was asked yet.
    enum class GrantsState : uint8_t
    {
        Unasked = 0,
        Asked,
    };
    GrantsState _grantsState = GrantsState::Unasked;
    std::optional<std::vector<std::string>> _grants; ///< The daemon's grants; nothing when unknown.
};

} // namespace contour::cli
