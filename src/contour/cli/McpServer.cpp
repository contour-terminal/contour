// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/McpServer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <ranges>
#include <span>
#include <utility>

#include <vtcontrol/Json.hpp>
#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/Target.hpp>

using vtcontrol::ObjectKind;

namespace contour::cli
{

namespace wire
{
    /// The JSON-RPC message, as far as routing needs it. Unknown members are ignored.
    struct Envelope
    {
        std::string jsonrpc {};
        vtcontrol::RawJson id {};
        std::string method {};
        vtcontrol::RawJson params {};
    };

    struct InitializeParams
    {
        std::string protocolVersion {};
    };

    struct CallParams
    {
        std::string name {};
        vtcontrol::RawJson arguments {};
    };
} // namespace wire

namespace
{
    using namespace wire;
    constexpr auto CallTimeout = std::chrono::seconds { 30 };
    constexpr auto ParseError = -32700;
    constexpr auto InvalidRequest = -32600;
    constexpr auto MethodNotFound = -32601;
    constexpr auto InvalidParams = -32602;
    constexpr auto OutsideHint =
        std::string_view { "; start `contour mcp` with --allow-all-tabs to reach it" };
    constexpr auto WouldCloseSelf = std::string_view { "would close this agent" };

    constexpr auto LenientOptions =
        glz::opts { .error_on_unknown_keys = false, .error_on_missing_keys = false };

    template <typename T>
    std::optional<T> readLenient(std::string_view text)
    {
        auto value = T {};
        if (glz::read<LenientOptions>(value, text.empty() ? std::string_view { "{}" } : text))
            return std::nullopt;
        return value;
    }

    std::string textResult(std::string_view text, std::string_view isError)
    {
        return std::format(R"({{"content":[{{"type":"text","text":{}}}],"isError":{}}})",
                           vtcontrol::quoteJsonString(text),
                           isError);
    }

    std::string reply(std::string_view id, std::string_view member)
    {
        return std::format(R"({{"jsonrpc":"2.0","id":{},{}}})", id, member);
    }

    std::string errorReply(std::string_view id, int code, std::string_view message)
    {
        return reply(
            id,
            std::format(R"("error":{{"code":{},"message":{}}})", code, vtcontrol::quoteJsonString(message)));
    }

    std::string callerGoneText(std::string_view callerPane)
    {
        return std::format("{} is not in the workspace any more", callerPane);
    }

    /// @return The pane id a block id (`block:<pane uuid>/<n>`) belongs to; empty when it is not one.
    std::string paneOfBlock(std::string_view block)
    {
        constexpr auto Prefix = std::string_view { "block:" };
        if (!block.starts_with(Prefix))
            return {};
        auto const uuid =
            block.substr(Prefix.size(),
                         block.find('/') == std::string_view::npos ? std::string_view::npos
                                                                   : block.find('/') - Prefix.size());
        return uuid.empty() ? std::string {} : "pane:" + std::string(uuid);
    }

    /// @return The tabs or windows of @p workspace as target candidates; with @p onlyTab, just that tab.
    std::vector<vtcontrol::TargetCandidate> candidatesOf(vtcontrol::ListReply const& workspace,
                                                         ObjectKind kind,
                                                         std::string_view onlyTab)
    {
        auto out = std::vector<vtcontrol::TargetCandidate> {};
        auto const add = [&](std::string const& id, std::string const& label, uint32_t position) {
            if (auto const ref = vtcontrol::parseObjectRef(id))
                out.push_back({ .ref = *ref, .label = label, .position = position });
        };
        for (auto const& window: workspace.windows)
        {
            if (kind == ObjectKind::Window)
                add(window.id, window.label, window.position);
            else
                for (auto const& tab: window.tabs)
                    if (onlyTab.empty() || tab.id == onlyTab)
                        add(tab.id, tab.label, tab.position);
        }
        return out;
    }

    /// Resolves a tab or window spec to a full id, under the same scoping rules as resolvePane.
    /// @param spec What the agent wrote; empty names the caller's own tab.
    std::expected<std::string, std::string> resolveContainer(vtcontrol::ListReply const& workspace,
                                                             std::string_view callerPane,
                                                             ObjectKind kind,
                                                             std::string_view spec,
                                                             TabScope scope)
    {
        auto const ownTab = ownTabOf(workspace, callerPane).value_or(std::string {});
        if (kind == ObjectKind::Tab && spec.empty())
        {
            if (ownTab.empty())
                return std::unexpected(std::string { "no caller pane is known (CONTOUR_PANE is not set), "
                                                     "so there is no default tab; name one" });
            return ownTab;
        }
        if (kind == ObjectKind::Window && scope == TabScope::OwnTab)
            return std::unexpected(std::format("windows are out of reach{}", OutsideHint));

        auto const all = candidatesOf(workspace, kind, {});
        auto const inScope =
            scope == TabScope::AllTabs ? all : candidatesOf(workspace, kind, ownTab.empty() ? "-" : ownTab);
        auto const found = vtcontrol::resolveTarget(kind, spec, inScope);
        if (found)
            return vtcontrol::toString(*found);
        if (scope == TabScope::OwnTab && found.error().code != vtcontrol::ControlErrorCode::Ambiguous
            && vtcontrol::resolveTarget(kind, spec, all))
            return std::unexpected(std::format("'{}' is in another tab{}", spec, OutsideHint));
        return std::unexpected(found.error().message);
    }

    template <typename Request>
    std::expected<Request, std::string> decodeArguments(std::string_view arguments)
    {
        auto decoded = vtcontrol::decodeJson<Request>(arguments);
        if (!decoded)
            return std::unexpected(decoded.error().message);
        return *std::move(decoded);
    }
} // namespace

McpServer::McpServer(ControlChannel& channel, McpOptions options):
    _channel { channel }, _options { std::move(options) }
{
}

std::optional<std::string> McpServer::handleLine(std::string_view line)
{
    auto const first = line.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return std::nullopt;
    if (line[first] == '[')
        return errorReply("null", InvalidRequest, "batch requests are not supported");

    auto document = glz::generic {};
    if (glz::read_json(document, line))
        return errorReply("null", ParseError, "parse error");
    auto const envelope =
        document.get_if<glz::generic::object_t>() != nullptr ? readLenient<Envelope>(line) : std::nullopt;
    if (!envelope || envelope->method.empty())
        return errorReply("null", InvalidRequest, "invalid request");

    auto const request =
        Request { .id = envelope->id.text, .method = envelope->method, .params = envelope->params.text };
    auto const result = dispatch(request);
    if (request.id.empty())
        return std::nullopt; // a notification never gets an answer
    if (!result)
        return errorReply(request.id, result.error().code, result.error().message);
    return reply(request.id, std::format(R"("result":{})", *result));
}

McpServer::RpcResult McpServer::dispatch(Request const& request)
{
    using Handler = RpcResult (McpServer::*)(Request const&);
    struct Row
    {
        std::string_view name;
        Handler handler;
    };
    static constexpr auto Methods = std::to_array<Row>({
        { "initialize", &McpServer::onInitialize },
        { "notifications/initialized", &McpServer::onInitialized },
        { "ping", &McpServer::onPing },
        { "tools/list", &McpServer::onToolsList },
        { "tools/call", &McpServer::onToolsCall },
    });
    auto const row = std::ranges::find(Methods, request.method, &Row::name);
    if (row == Methods.end())
        return std::unexpected(
            RpcError { .code = MethodNotFound, .message = "method not found: " + request.method });
    return (this->*(row->handler))(request);
}

McpServer::RpcResult McpServer::onInitialize(Request const& request)
{
    auto const params = readLenient<InitializeParams>(request.params);
    if (!params)
        return std::unexpected(RpcError { .code = InvalidParams, .message = "invalid initialize params" });
    auto const supported = std::ranges::find(McpProtocolVersions, params->protocolVersion);
    auto const version = supported != McpProtocolVersions.end() ? *supported : McpProtocolVersions.front();
    learnGrants();
    return std::format(
        R"({{"protocolVersion":"{}","capabilities":{{"tools":{{}}}},"serverInfo":{{"name":"contour","version":{}}}}})",
        version,
        vtcontrol::quoteJsonString(_options.serverVersion));
}

McpServer::RpcResult McpServer::onInitialized(Request const& /*request*/)
{
    return "{}";
}

McpServer::RpcResult McpServer::onPing(Request const& /*request*/)
{
    return "{}";
}

McpServer::RpcResult McpServer::onToolsList(Request const& /*request*/)
{
    return toolsListJson(_grants, _options.access, _options.scope);
}

McpServer::RpcResult McpServer::onToolsCall(Request const& request)
{
    auto const params = readLenient<CallParams>(request.params);
    if (!params || params->name.empty())
        return std::unexpected(RpcError { .code = InvalidParams, .message = "tools/call needs a tool name" });
    auto const rows = mcpToolRows();
    auto const row = std::ranges::find(rows, params->name, [](ToolRow const& r) { return r.name; });
    if (row == rows.end())
        return std::unexpected(
            RpcError { .code = InvalidParams, .message = "unknown tool: " + params->name });
    auto const arguments = params->arguments.text.empty() ? std::string { "{}" } : params->arguments.text;
    auto const result = runTool(*row, arguments);
    return textResult(result.text, result.outcome == ToolOutcome::Ok ? "false" : "true");
}

void McpServer::learnGrants()
{
    if (_grantsState == GrantsState::Asked)
        return;
    _grantsState = GrantsState::Asked;
    auto const answer = _channel.call("daemon.info", "{}", _options.callerPane, CallTimeout);
    if (!answer)
        return;
    auto const info = vtcontrol::decodeJson<vtcontrol::DaemonInfoReply>(*answer);
    if (info && !info->grants.empty())
        _grants = info->grants;
}

std::expected<vtcontrol::ListReply, McpServer::Failure> McpServer::fetchWorkspace()
{
    auto const answer = _channel.call("workspace.list", "{}", _options.callerPane, CallTimeout);
    if (!answer)
        return std::unexpected(answer.error().message);
    auto workspace = vtcontrol::decodeJson<vtcontrol::ListReply>(*answer);
    if (!workspace)
        return std::unexpected(workspace.error().message);
    // The agent's own pane has gone: never fall back to another pane.
    if (!_options.callerPane.empty() && !ownTabOf(*workspace, _options.callerPane))
        return std::unexpected(callerGoneText(_options.callerPane));
    return *std::move(workspace);
}

McpServer::ToolResult McpServer::callDaemon(std::string_view method, std::string const& params)
{
    auto const answer = _channel.call(method, params, _options.callerPane, CallTimeout);
    if (!answer)
        return { .outcome = ToolOutcome::Error, .text = answer.error().message };
    return { .outcome = ToolOutcome::Ok, .text = *answer };
}

McpServer::ToolResult McpServer::runTool(ToolRow const& row, std::string_view arguments)
{
    using Handler = ToolResult (McpServer::*)(ToolRow const&, std::string_view);
    struct Entry
    {
        std::string_view name;
        Handler handler;
    };
    // A catalog tool is a row here and a row in mcpToolRows(); composite tools join in the same way.
    static constexpr auto Handlers = std::to_array<Entry>({
        { "workspace_list", &McpServer::toolWorkspaceList },
        { "pane_capture", &McpServer::toolOnPane<vtcontrol::CaptureRequest> },
        { "pane_close", &McpServer::toolPaneClose },
        { "tab_close", &McpServer::toolTabClose },
        { "object_setLabel", &McpServer::toolSetLabel },
        { "block_list", &McpServer::toolOnPane<vtcontrol::BlockListRequest> },
        { "block_read", &McpServer::toolBlockRead },
        { "daemon_info", &McpServer::toolDaemonInfo },
    });

    auto const refuse = [](std::string text) {
        return ToolResult { .outcome = ToolOutcome::Error, .text = std::move(text) };
    };
    if (_options.access == McpAccess::ReadOnly
        && (row.needs.contains(vtcontrol::Permission::Input)
            || row.needs.contains(vtcontrol::Permission::Write)))
        return refuse(std::format("{} is not available: the server is read-only", row.name));
    if (row.requiredScope == TabScope::AllTabs && _options.scope != TabScope::AllTabs)
        return refuse(std::format("{} is not available{}", row.name, OutsideHint));
    auto const entry = std::ranges::find(Handlers, row.name, &Entry::name);
    if (entry == Handlers.end())
        return refuse(std::format("{} is not available", row.name));
    return (this->*(entry->handler))(row, arguments);
}

McpServer::ToolResult McpServer::toolWorkspaceList(ToolRow const& /*row*/, std::string_view /*arguments*/)
{
    auto const workspace = fetchWorkspace();
    if (!workspace)
        return { .outcome = ToolOutcome::Error, .text = workspace.error() };
    return { .outcome = ToolOutcome::Ok, .text = annotateWorkspace(*workspace, _options.callerPane) };
}

McpServer::ToolResult McpServer::toolDaemonInfo(ToolRow const& row, std::string_view /*arguments*/)
{
    return callDaemon(row.catalogMethod, "{}");
}

template <typename Request>
McpServer::ToolResult McpServer::toolOnPane(ToolRow const& row, std::string_view arguments)
{
    auto request = decodeArguments<Request>(arguments);
    if (!request)
        return { .outcome = ToolOutcome::Error, .text = request.error() };
    auto const workspace = fetchWorkspace();
    if (!workspace)
        return { .outcome = ToolOutcome::Error, .text = workspace.error() };
    auto const pane = resolvePane(*workspace, _options.callerPane, request->pane, _options.scope);
    if (!pane)
        return { .outcome = ToolOutcome::Error, .text = pane.error().message };
    request->pane = *pane; // the resolved full id, never the agent's text
    return callDaemon(row.catalogMethod, vtcontrol::encodeJson(*request));
}

McpServer::ToolResult McpServer::toolPaneClose(ToolRow const& row, std::string_view arguments)
{
    auto request = decodeArguments<vtcontrol::ClosePaneRequest>(arguments);
    if (!request)
        return { .outcome = ToolOutcome::Error, .text = request.error() };
    auto const workspace = fetchWorkspace();
    if (!workspace)
        return { .outcome = ToolOutcome::Error, .text = workspace.error() };
    auto const pane = resolvePane(*workspace, _options.callerPane, request->pane, _options.scope);
    if (!pane)
        return { .outcome = ToolOutcome::Error, .text = pane.error().message };
    if (*pane == _options.callerPane)
        return { .outcome = ToolOutcome::Error, .text = std::string(WouldCloseSelf) };
    request->pane = *pane;
    return callDaemon(row.catalogMethod, vtcontrol::encodeJson(*request));
}

McpServer::ToolResult McpServer::toolTabClose(ToolRow const& row, std::string_view arguments)
{
    auto request = decodeArguments<vtcontrol::CloseTabRequest>(arguments);
    if (!request)
        return { .outcome = ToolOutcome::Error, .text = request.error() };
    auto const workspace = fetchWorkspace();
    if (!workspace)
        return { .outcome = ToolOutcome::Error, .text = workspace.error() };
    auto const tab =
        resolveContainer(*workspace, _options.callerPane, ObjectKind::Tab, request->tab, _options.scope);
    if (!tab)
        return { .outcome = ToolOutcome::Error, .text = tab.error() };
    if (ownTabOf(*workspace, _options.callerPane) == *tab)
        return { .outcome = ToolOutcome::Error, .text = std::string(WouldCloseSelf) };
    request->tab = *tab;
    return callDaemon(row.catalogMethod, vtcontrol::encodeJson(*request));
}

McpServer::ToolResult McpServer::toolSetLabel(ToolRow const& row, std::string_view arguments)
{
    auto request = decodeArguments<vtcontrol::SetLabelRequest>(arguments);
    if (!request)
        return { .outcome = ToolOutcome::Error, .text = request.error() };
    auto const fail = [](std::string text) {
        return ToolResult { .outcome = ToolOutcome::Error, .text = std::move(text) };
    };
    auto const colon = request->object.find(':');
    auto const kind =
        colon == std::string::npos
            ? std::nullopt
            : vtcontrol::enumFromName<ObjectKind>(std::string_view { request->object }.substr(0, colon));
    if (!kind)
        return fail(std::format("'{}' names no window, tab or pane; write <kind>:<target>", request->object));
    auto const spec = vtcontrol::parseObjectRef(request->object)
                          ? std::string_view { request->object }
                          : std::string_view { request->object }.substr(colon + 1);

    auto const workspace = fetchWorkspace();
    if (!workspace)
        return fail(workspace.error());
    auto const resolved =
        *kind == ObjectKind::Pane
            ? resolvePane(*workspace, _options.callerPane, spec, _options.scope)
                  .transform_error([](ScopeError const& error) { return error.message; })
            : resolveContainer(*workspace, _options.callerPane, *kind, spec, _options.scope);
    if (!resolved)
        return fail(resolved.error());
    request->object = *resolved; // a full id, which the daemon takes as is
    return callDaemon(row.catalogMethod, vtcontrol::encodeJson(*request));
}

McpServer::ToolResult McpServer::toolBlockRead(ToolRow const& row, std::string_view arguments)
{
    auto request = decodeArguments<vtcontrol::BlockReadRequest>(arguments);
    if (!request)
        return { .outcome = ToolOutcome::Error, .text = request.error() };
    auto const fail = [](std::string text) {
        return ToolResult { .outcome = ToolOutcome::Error, .text = std::move(text) };
    };
    // A block belongs to the pane its id names; that pane is what the scope is judged on.
    auto target = std::string_view { request->pane };
    auto blockPane = std::string {};
    if (!request->block.empty())
    {
        blockPane = paneOfBlock(request->block);
        if (blockPane.empty())
            return fail("block must be a full block id (block:<pane uuid>/<n>)");
        target = blockPane;
    }
    auto const workspace = fetchWorkspace();
    if (!workspace)
        return fail(workspace.error());
    auto const pane = resolvePane(*workspace, _options.callerPane, target, _options.scope);
    if (!pane)
        return fail(pane.error().message);
    request->pane = *pane;
    return callDaemon(row.catalogMethod, vtcontrol::encodeJson(*request));
}

} // namespace contour::cli
