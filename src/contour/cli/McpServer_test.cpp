// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp` server core: the JSON-RPC envelope, the scope rules and the catalog tools, over a channel
// that answers from a script and records what it was asked.

#include <contour/cli/McpServer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/Json.hpp>

using namespace std::string_literals;
using contour::cli::McpOptions;
using contour::cli::McpServer;
using contour::cli::TabScope;
using vthost::client::ControlClientError;
using vthost::client::ControlFailure;

namespace
{
constexpr auto A1 = "pane:018f3c4e-0000-7000-8000-00000000a001";
constexpr auto A2 = "pane:018f3c4e-0000-7000-8000-00000000a002";
constexpr auto B1 = "pane:018f3c4e-0000-7000-8000-00000000b001";
constexpr auto TabA = "tab:018f3c4e-0000-7000-8000-0000000000aa";
constexpr auto TabB = "tab:018f3c4e-0000-7000-8000-0000000000bb";

vtcontrol::ListReply workspace()
{
    auto tabA = vtcontrol::TabInfo { .id = TabA, .position = 1 };
    tabA.panes.push_back(vtcontrol::PaneInfo { .id = A1, .label = "build" });
    tabA.panes.push_back(vtcontrol::PaneInfo { .id = A2, .label = "tests" });
    auto tabB = vtcontrol::TabInfo { .id = TabB, .position = 2 };
    tabB.panes.push_back(vtcontrol::PaneInfo { .id = B1, .label = "other" });
    auto window =
        vtcontrol::WindowInfo { .id = "window:018f3c4e-0000-7000-8000-0000000000ff", .position = 1 };
    window.tabs.push_back(tabA);
    window.tabs.push_back(tabB);
    return { .windows = { window } };
}

/// Answers each method with its scripted reply (`{}` when none) and records every call.
class ScriptedChannel final: public contour::cli::ControlChannel
{
  public:
    struct Call
    {
        std::string method;
        std::string params;
        std::string context;
        std::optional<std::chrono::milliseconds> timeout;
    };

    std::expected<std::string, ControlClientError> call(
        std::string_view method,
        std::string_view params,
        std::string_view context,
        std::optional<std::chrono::milliseconds> timeout) override
    {
        calls.push_back({ std::string(method), std::string(params), std::string(context), timeout });
        if (auto const failure = failures.find(std::string(method)); failure != failures.end())
            return std::unexpected(failure->second);
        if (auto const it = replies.find(std::string(method)); it != replies.end())
            return it->second;
        return "{}"s;
    }

    std::expected<void, ControlClientError> stream(
        std::string_view,
        std::string_view,
        std::function<vthost::client::StreamStep(std::string_view)>,
        core::async::StopToken const&) override
    {
        return {};
    }

    [[nodiscard]] int count(std::string_view method) const
    {
        auto n = 0;
        for (auto const& c: calls)
            n += c.method == method ? 1 : 0;
        return n;
    }

    [[nodiscard]] Call const* last(std::string_view method) const
    {
        auto const reversed = calls | std::views::reverse;
        auto const found = std::ranges::find(reversed, method, &Call::method);
        return found == reversed.end() ? nullptr : &*found;
    }

    std::vector<Call> calls;
    std::map<std::string, std::string> replies;
    std::map<std::string, ControlClientError> failures;
};

struct Fixture
{
    ScriptedChannel channel;
    McpServer server;

    explicit Fixture(McpOptions options = { .callerPane = A1 }): server { channel, std::move(options) }
    {
        channel.replies["workspace.list"] = vtcontrol::encodeJson(workspace());
    }

    std::string call(std::string_view tool, std::string_view arguments = "{}")
    {
        auto const line = std::format(
            R"({{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{{"name":"{}","arguments":{}}}}})",
            tool,
            arguments);
        return server.handleLine(line).value_or("");
    }
};
} // namespace

TEST_CASE("initialize echoes a supported version and falls back to the newest", "[mcp-server]")
{
    auto f = Fixture {};
    auto const echoed = f.server.handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})");
    REQUIRE(echoed);
    CHECK(echoed->contains(R"("protocolVersion":"2024-11-05")"));
    CHECK(echoed->contains(R"("tools":{})"));
    CHECK(echoed->contains(R"("serverInfo")"));

    auto const fallback = f.server.handleLine(
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"1999-01-01"}})");
    REQUIRE(fallback);
    CHECK(fallback->contains(R"("protocolVersion":"2025-06-18")"));
    CHECK(f.channel.count("daemon.info") == 1); // once, not per initialize
}

TEST_CASE("notifications get no reply and ping answers {}", "[mcp-server]")
{
    auto f = Fixture {};
    CHECK_FALSE(f.server.handleLine(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    CHECK_FALSE(f.server.handleLine(R"({"jsonrpc":"2.0","method":"nope"})"));
    auto const pong = f.server.handleLine(R"({"jsonrpc":"2.0","id":3,"method":"ping"})");
    REQUIRE(pong);
    CHECK(pong->contains(R"("id":3)"));
    CHECK(pong->contains(R"("result":{})"));
}

TEST_CASE("malformed input is a JSON-RPC error", "[mcp-server]")
{
    auto f = Fixture {};
    auto const garbled = f.server.handleLine("{not json");
    REQUIRE(garbled);
    CHECK(garbled->contains(R"("id":null)"));
    CHECK(garbled->contains("-32700"));
    CHECK(f.server.handleLine(R"({"jsonrpc":"2.0","id":1,"method":"nope"})")->contains("-32601"));
    CHECK(f.server.handleLine(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])")->contains("-32600"));
    CHECK(f.server.handleLine(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{}})")
              ->contains("-32602"));
    CHECK(f.server.handleLine(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"nope"}})")
              ->contains("-32602"));
}

TEST_CASE("a daemon that reports no grants still lists the tools", "[mcp-server]")
{
    auto f = Fixture {};
    f.channel.replies["daemon.info"] = R"({"version":"1","protocolVersion":1})";
    REQUIRE(f.server.handleLine(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"));
    auto const list = f.server.handleLine(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    REQUIRE(list);
    CHECK(list->contains("pane_capture"));
    CHECK(list->contains("pane_run"));
}

TEST_CASE("workspace_list is annotated", "[mcp-server]")
{
    auto f = Fixture {};
    auto const out = f.call("workspace_list");
    CHECK(out.contains(R"("isError":false)"));
    CHECK(out.contains("isSelf"));
    CHECK(out.contains("isOwnTab"));
}

TEST_CASE("pane_capture defaults to the caller and sends full ids", "[mcp-server]")
{
    auto f = Fixture {};
    CHECK(f.call("pane_capture").contains(R"("isError":false)"));
    REQUIRE(f.channel.last("pane.capture"));
    CHECK(f.channel.last("pane.capture")->params.contains(A1));
    CHECK(f.channel.last("pane.capture")->context == A1);

    CHECK(f.call("pane_capture", R"({"pane":"tests"})").contains(R"("isError":false)"));
    CHECK(f.channel.last("pane.capture")->params.contains(A2)); // the label became a full id
    CHECK_FALSE(f.channel.last("pane.capture")->params.contains("tests"));
}

TEST_CASE("OwnTab refuses another tab's pane without calling the daemon", "[mcp-server]")
{
    auto f = Fixture {};
    for (auto const* target: { B1, "other" })
    {
        auto const out = f.call("pane_capture", std::format(R"({{"pane":"{}"}})", target));
        CHECK(out.contains(R"("isError":true)"));
        CHECK(out.contains("--allow-all-tabs"));
    }
    CHECK(f.channel.count("pane.capture") == 0);
    CHECK(f.call("block_list", std::format(R"({{"pane":"{}"}})", B1)).contains(R"("isError":true)"));
    CHECK(f.channel.count("block.list") == 0);
}

TEST_CASE("AllTabs reaches other tabs", "[mcp-server]")
{
    auto f = Fixture { { .scope = TabScope::AllTabs, .callerPane = A1 } };
    CHECK(f.call("pane_capture", R"({"pane":"other"})").contains(R"("isError":false)"));
    CHECK(f.channel.last("pane.capture")->params.contains(B1));
}

TEST_CASE("a vanished caller pane fails tools instead of falling back", "[mcp-server]")
{
    auto f = Fixture { { .callerPane = "pane:018f3c4e-0000-7000-8000-00000000ffff" } };
    auto const out = f.call("pane_capture");
    CHECK(out.contains(R"("isError":true)"));
    CHECK(out.contains("not in the workspace any more"));
    CHECK(f.channel.count("pane.capture") == 0);
    CHECK(f.call("workspace_list").contains(R"("isError":true)"));
}

TEST_CASE("an agent cannot close itself", "[mcp-server]")
{
    auto f = Fixture { { .scope = TabScope::AllTabs, .callerPane = A1 } };
    CHECK(f.call("pane_close").contains("would close this agent"));
    CHECK(f.call("pane_close", R"({"pane":"build"})").contains("would close this agent"));
    CHECK(f.call("tab_close").contains("would close this agent"));
    CHECK(f.call("tab_close", std::format(R"({{"tab":"{}"}})", TabA)).contains("would close this agent"));
    CHECK(f.channel.count("pane.close") == 0);
    CHECK(f.channel.count("tab.close") == 0);

    CHECK(f.call("pane_close", R"({"pane":"tests"})").contains(R"("isError":false)"));
    CHECK(f.channel.last("pane.close")->params.contains(A2));
    CHECK(f.call("tab_close", std::format(R"({{"tab":"{}"}})", TabB)).contains(R"("isError":false)"));
    CHECK(f.channel.last("tab.close")->params.contains(TabB));
}

TEST_CASE("OwnTab will not close another tab", "[mcp-server]")
{
    auto f = Fixture {};
    CHECK(f.call("tab_close", std::format(R"({{"tab":"{}"}})", TabB)).contains("--allow-all-tabs"));
    CHECK(f.channel.count("tab.close") == 0);
}

TEST_CASE("object_setLabel is confined to the own tab", "[mcp-server]")
{
    auto f = Fixture {};
    CHECK(f.call("object_setLabel", R"({"object":"pane:tests","label":"t"})").contains(R"("isError":false)"));
    CHECK(f.channel.last("object.setLabel")->params.contains(A2));
    CHECK(f.call("object_setLabel", std::format(R"({{"object":"{}","label":"t"}})", TabA))
              .contains(R"("isError":false)"));
    f.channel.calls.clear();
    CHECK(f.call("object_setLabel", std::format(R"({{"object":"{}","label":"t"}})", B1))
              .contains(R"("isError":true)"));
    CHECK(f.call("object_setLabel", std::format(R"({{"object":"{}","label":"t"}})", TabB))
              .contains(R"("isError":true)"));
    CHECK(f.call("object_setLabel", R"({"object":"window:1","label":"t"})").contains(R"("isError":true)"));
    CHECK(f.call("object_setLabel", R"({"object":"bogus","label":"t"})").contains(R"("isError":true)"));
    CHECK(f.channel.count("object.setLabel") == 0);
}

TEST_CASE("block_read scopes a block by its pane", "[mcp-server]")
{
    auto f = Fixture {};
    CHECK(f.call("block_read").contains(R"("isError":false)"));
    CHECK(f.channel.last("block.read")->params.contains(A1));
    f.channel.calls.clear();
    auto const other = std::format(R"({{"block":"block:{}/4"}})", std::string(B1).substr(5));
    CHECK(f.call("block_read", other).contains(R"("isError":true)"));
    CHECK(f.call("block_read", R"({"block":"junk"})").contains(R"("isError":true)"));
    CHECK(f.channel.count("block.read") == 0);
}

TEST_CASE("daemon failures are tool errors, not JSON-RPC errors", "[mcp-server]")
{
    auto f = Fixture {};
    f.channel.failures["pane.capture"] = { .failure = ControlFailure::TimedOut,
                                           .message = "timed out",
                                           .remote = {} };
    auto const out = f.call("pane_capture");
    CHECK(out.contains(R"("isError":true)"));
    CHECK(out.contains("timed out"));
    CHECK_FALSE(out.contains(R"("error":{)"));
}

TEST_CASE("read-only access refuses mutating tools", "[mcp-server]")
{
    auto f = Fixture { { .access = contour::cli::McpAccess::ReadOnly, .callerPane = A1 } };
    CHECK(f.call("pane_close", R"({"pane":"tests"})").contains(R"("isError":true)"));
    CHECK(f.channel.count("pane.close") == 0);
    CHECK(f.call("daemon_info").contains(R"("isError":false)"));
}

namespace
{
constexpr auto RunBlock = "block:018f3c4e-0000-7000-8000-00000000a001/3";

std::string blockJson(std::string_view state, std::string_view exitCode = "0")
{
    return std::format(R"({{"id":"{}","pane":"{}","state":"{}","exitCode":{},"durationMs":42}})",
                       RunBlock,
                       A1,
                       state,
                       exitCode);
}

/// A pane whose newest block is finished, whose command finishes with exit code 0 and prints "hello".
void scriptRun(Fixture& f)
{
    f.channel.replies["block.list"] =
        std::format(R"({{"pane":"{}","blocks":[{}]}})", A1, blockJson("finished"));
    f.channel.replies["pane.send"] = std::format(R"({{"block":{}}})", blockJson("finished"));
    f.channel.replies["block.read"] = std::format(
        R"({{"block":{},"prompt":"","output":"hello\n","truncated":"none"}})", blockJson("finished"));
}

std::vector<std::string> methodsOf(Fixture const& f)
{
    auto out = std::vector<std::string> {};
    for (auto const& c: f.channel.calls)
        if (c.method != "workspace.list")
            out.push_back(c.method);
    return out;
}
} // namespace

TEST_CASE("pane_split keeps the focus and sends the resolved full id", "[mcp-server]")
{
    auto f = Fixture {};
    auto const out =
        f.call("pane_split",
               R"({"pane":"tests","direction":"below","ratioPercent":30,"label":"x","command":["ls"]})");
    CHECK(out.contains(R"("isError":false)"));
    auto const* split = f.channel.last("pane.split");
    REQUIRE(split);
    CHECK(split->params.contains(A2));
    CHECK_FALSE(split->params.contains("tests"));
    CHECK(split->params.contains(R"("focus":"keep")"));
    CHECK(split->params.contains(R"("direction":"below")"));
    CHECK(split->params.contains(R"("ratioPercent":30)"));
    CHECK(split->context == A1);
}

TEST_CASE("pane_type sends the text as is, with no Enter and no waiting", "[mcp-server]")
{
    auto f = Fixture {};
    CHECK(f.call("pane_type", R"({"pane":"tests","text":"ls"})").contains(R"("isError":false)"));
    auto const* send = f.channel.last("pane.send");
    REQUIRE(send);
    CHECK(send->params.contains(A2));
    CHECK(send->params.contains(R"("text":"ls")"));
    CHECK_FALSE(send->params.contains(R"(\r)"));
    CHECK(send->params.contains(R"("wait":"none")"));
}

TEST_CASE("pane_run lists, sends and reads in order and reports the exit code", "[mcp-server]")
{
    auto f = Fixture {};
    scriptRun(f);
    auto const out = f.call("pane_run", R"({"pane":"tests","command":"echo hello"})");
    CHECK(out.contains(R"("isError":false)"));
    CHECK(out.contains(R"(\"exitCode\":0)"));
    CHECK(out.contains("hello"));
    CHECK(out.contains(R"(\"truncated\":\"none\")"));
    CHECK(out.contains(RunBlock));
    CHECK(methodsOf(f) == std::vector<std::string> { "block.list", "pane.send", "block.read" });
    auto const* list = f.channel.last("block.list");
    CHECK(list->params.contains(R"("limit":1)"));
    auto const* send = f.channel.last("pane.send");
    CHECK(send->params.contains(R"("text":"echo hello\r")"));
    CHECK(send->params.contains(R"("wait":"commandFinished")"));
    CHECK(send->params.contains(A2));
    CHECK(send->timeout == std::chrono::seconds { 120 });
    CHECK(f.channel.last("block.read")->params.contains(R"("part":"output")"));
    CHECK(f.channel.last("block.read")->params.contains(RunBlock));

    CHECK(f.call("pane_run", R"({"pane":"tests","command":"x","timeoutSeconds":5})")
              .contains(R"("isError":false)"));
    CHECK(f.channel.last("pane.send")->timeout == std::chrono::seconds { 5 });
}

TEST_CASE("pane_run on a busy pane types nothing", "[mcp-server]")
{
    auto f = Fixture {};
    scriptRun(f);
    f.channel.replies["block.list"] =
        std::format(R"({{"pane":"{}","blocks":[{}]}})", A1, blockJson("running"));
    auto const out = f.call("pane_run", R"({"pane":"tests","command":"echo hello"})");
    CHECK(out.contains(R"("isError":true)"));
    CHECK(out.contains("pane_type"));
    CHECK(f.channel.count("pane.send") == 0);
}

TEST_CASE("pane_run that times out reports timedOut and reads nothing", "[mcp-server]")
{
    auto f = Fixture {};
    scriptRun(f);
    f.channel.failures["pane.send"] = { .failure = ControlFailure::TimedOut,
                                        .message = "timed out",
                                        .remote = {} };
    auto const out = f.call("pane_run", R"({"pane":"tests","command":"sleep 999"})");
    CHECK(out.contains(R"("isError":false)"));
    CHECK(out.contains(R"(\"timedOut\":true)"));
    CHECK(f.channel.count("block.read") == 0);

    f.channel.failures["pane.send"] = { .failure = ControlFailure::Disconnected,
                                        .message = "gone",
                                        .remote = {} };
    CHECK(f.call("pane_run", R"({"pane":"tests","command":"x"})").contains(R"("isError":true)"));
}

TEST_CASE("pane_run with a superseded or exit-less block is an error", "[mcp-server]")
{
    auto f = Fixture {};
    scriptRun(f);
    f.channel.replies["pane.send"] = std::format(R"({{"block":{}}})", blockJson("superseded"));
    auto out = f.call("pane_run", R"({"pane":"tests","command":"x"})");
    CHECK(out.contains(R"("isError":true)"));
    CHECK(out.contains("superseded"));
    f.channel.replies["pane.send"] =
        std::format(R"({{"block":{{"id":"{}","pane":"{}","state":"finished"}}}})", RunBlock, A1);
    CHECK(f.call("pane_run", R"({"pane":"tests","command":"x"})").contains(R"("isError":true)"));
    CHECK(f.channel.count("block.read") == 0);
}

TEST_CASE("pane_run and pane_capture keep the tail of long output", "[mcp-server]")
{
    auto f = Fixture {};
    scriptRun(f);
    auto const big = std::string(200 * 1024, 'a') + "TAIL";
    f.channel.replies["block.read"] = std::format(
        R"({{"block":{},"prompt":"","output":"{}","truncated":"none"}})", blockJson("finished"), big);
    auto const out = f.call("pane_run", R"({"pane":"tests","command":"x"})");
    CHECK(out.contains(R"(\"truncated\":\"head\")"));
    CHECK(out.contains("TAIL"));
    CHECK(out.size() < 70 * 1024);
    CHECK(f.call("pane_run", R"({"pane":"tests","command":"x","maxOutputBytes":4})")
              .contains(R"(\"output\":\"TAIL\")"));

    f.channel.replies["pane.capture"] = std::format(
        R"({{"pane":"{}","text":"{}","truncated":"none","cursor":{{"line":1,"column":1}},"size":{{"lines":24,"columns":80}}}})",
        A1,
        big);
    auto const capture = f.call("pane_capture");
    CHECK(capture.contains(R"(\"truncated\":\"head\")"));
    CHECK(capture.contains("TAIL"));
    CHECK(capture.size() < 70 * 1024);
}

TEST_CASE("tab_create keeps the focus and needs AllTabs", "[mcp-server]")
{
    auto own = Fixture {};
    CHECK(own.call("tab_create", R"({"label":"x"})").contains("--allow-all-tabs"));
    CHECK(own.channel.count("tab.create") == 0);

    auto all = Fixture { { .scope = TabScope::AllTabs, .callerPane = A1 } };
    CHECK(all.call("tab_create", R"({"cwd":"/tmp","label":"x","command":["sh"]})")
              .contains(R"("isError":false)"));
    auto const* create = all.channel.last("tab.create");
    REQUIRE(create);
    CHECK(create->params.contains(R"("focus":"keep")"));
    CHECK(create->params.contains("/tmp"));
}

TEST_CASE("a missing required argument is -32602", "[mcp-server]")
{
    auto f = Fixture {};
    CHECK(f.call("pane_split").contains("-32602"));
    CHECK(f.call("pane_type").contains("-32602"));
    CHECK(f.call("pane_run").contains("-32602"));
    CHECK(f.call("pane_run", R"({"pane":"tests","command":"x","bogus":1})").contains("-32602"));
    CHECK(f.channel.count("pane.split") + f.channel.count("pane.send") == 0);
}

TEST_CASE("a tools/call notification runs nothing", "[mcp-server]")
{
    auto f = Fixture {};
    CHECK_FALSE(f.server.handleLine(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"pane_type","arguments":{"text":"rm -rf"}}})"));
    CHECK(f.channel.calls.empty());
}

TEST_CASE("tools/call refuses exactly what tools/list hides", "[mcp-server]")
{
    auto f = Fixture {};
    f.channel.replies["daemon.info"] = R"({"version":"1","protocolVersion":1,"grants":["read"]})";
    REQUIRE(f.server.handleLine(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"));
    f.channel.calls.clear();
    CHECK(f.call("pane_type", R"({"pane":"tests","text":"x"})").contains(R"("isError":true)"));
    CHECK(f.call("pane_run", R"({"pane":"tests","command":"x"})").contains(R"("isError":true)"));
    CHECK(f.call("pane_split", R"({"direction":"right"})").contains(R"("isError":true)"));
    CHECK(f.channel.calls.empty());
}

TEST_CASE("tab_create is refused by scope under OwnTab and allowed under AllTabs", "[mcp-server]")
{
    auto own = Fixture {};
    auto const refused = own.call("tab_create");
    CHECK(refused.contains(R"("isError":true)"));
    CHECK(refused.contains("--allow-all-tabs"));
    CHECK_FALSE(refused.contains("is not available\""));
    CHECK(own.channel.calls.empty());

    auto all = Fixture { { .scope = TabScope::AllTabs, .callerPane = A1 } };
    CHECK(all.call("tab_create").contains(R"("isError":false)"));
}

TEST_CASE("input-capable composites refuse a pane in another tab under OwnTab", "[mcp-server]")
{
    struct Tool
    {
        std::string_view name;
        std::string_view extra;
    };
    constexpr auto Tools = std::array {
        Tool { "pane_type", R"("text":"x")" },
        Tool { "pane_run", R"("command":"x")" },
        Tool { "pane_split", R"("direction":"right")" },
    };
    for (auto const& tool: Tools)
    {
        for (auto const* target: { B1, "other", "b001" })
        {
            auto f = Fixture {};
            scriptRun(f);
            auto const out = f.call(tool.name, std::format(R"({{"pane":"{}",{}}})", target, tool.extra));
            INFO(tool.name << " -> " << target);
            CHECK(out.contains(R"("isError":true)"));
            CHECK(methodsOf(f).empty());
        }
        for (auto const* target: { A2, "tests", "a002" })
        {
            auto f = Fixture {};
            scriptRun(f);
            auto const out = f.call(tool.name, std::format(R"({{"pane":"{}",{}}})", target, tool.extra));
            INFO(tool.name << " -> " << target);
            CHECK(out.contains(R"("isError":false)"));
            CHECK_FALSE(methodsOf(f).empty());
        }
    }
}

TEST_CASE("daemon replies with unknown keys are still read", "[mcp-server]")
{
    auto f = Fixture {};
    scriptRun(f);
    auto list = vtcontrol::encodeJson(workspace());
    list.insert(list.size() - 1, R"(,"addedLater":{"x":1})");
    f.channel.replies["workspace.list"] = list;
    f.channel.replies["pane.send"] = std::format(R"({{"block":{},"addedLater":1}})", blockJson("finished"));
    CHECK(f.call("workspace_list").contains(R"("isError":false)"));
    auto const out = f.call("pane_run", R"({"pane":"tests","command":"echo hello"})");
    CHECK(out.contains(R"("isError":false)"));
    CHECK(out.contains(R"(\"exitCode\":0)"));
}

TEST_CASE("pane_type and pane_run refuse the agent's own pane and need an explicit pane", "[mcp-server]")
{
    for (auto const* tool: { "pane_type", "pane_run" })
    {
        auto const extra = std::string_view { tool } == "pane_type" ? R"("text":"x")" : R"("command":"x")";
        for (auto const* target: { A1, "build", "a001" })
        {
            auto f = Fixture {};
            scriptRun(f);
            auto const out = f.call(tool, std::format(R"({{"pane":"{}",{}}})", target, extra));
            INFO(tool << " -> " << target);
            CHECK(out.contains(R"("isError":true)"));
            CHECK(out.contains("would type into this agent; use pane_split to get a shell pane"));
            CHECK(methodsOf(f).empty());
        }
        auto f = Fixture {};
        scriptRun(f);
        auto const out = f.call(tool, std::format("{{{}}}", extra));
        INFO(tool);
        CHECK(out.contains(R"("isError":true)"));
        CHECK(out.contains("pane is required"));
        CHECK(f.channel.calls.empty());
    }
}

TEST_CASE("pane_split and pane_capture still default to the agent's own pane", "[mcp-server]")
{
    auto f = Fixture {};
    CHECK(f.call("pane_split", R"({"direction":"right"})").contains(R"("isError":false)"));
    CHECK(f.channel.last("pane.split")->params.contains(A1));
}

TEST_CASE("pane_run failing after the command was sent says not to re-run it", "[mcp-server]")
{
    auto f = Fixture {};
    scriptRun(f);
    auto const expectGuard = [](std::string const& out) {
        CHECK(out.contains(R"("isError":true)"));
        CHECK(out.contains("the command ran; do not re-run it"));
        CHECK(out.contains("use block_read to fetch the output"));
        CHECK(out.contains(RunBlock));
        CHECK(out.contains("exitCode 0"));
        CHECK(out.contains("durationMs 42"));
    };
    f.channel.failures["block.read"] = { .failure = ControlFailure::Disconnected,
                                         .message = "gone",
                                         .remote = {} };
    expectGuard(f.call("pane_run", R"({"pane":"tests","command":"x"})"));

    f.channel.failures.clear();
    f.channel.replies["block.read"] = "not json";
    expectGuard(f.call("pane_run", R"({"pane":"tests","command":"x"})"));
}

TEST_CASE("pane_run takes one command line", "[mcp-server]")
{
    auto f = Fixture {};
    scriptRun(f);
    CHECK(f.call("pane_run", R"({"pane":"tests","command":"echo hi\n"})").contains(R"("isError":false)"));
    CHECK(f.channel.last("pane.send")->params.contains(R"("text":"echo hi\r")"));
    CHECK(f.call("pane_run", R"({"pane":"tests","command":"echo hi\r\n"})").contains(R"("isError":false)"));
    CHECK(f.channel.last("pane.send")->params.contains(R"("text":"echo hi\r")"));

    f.channel.calls.clear();
    for (auto const* command: { R"(a\nb)", R"(a\rb)", R"(\n)", "" })
    {
        auto const out = f.call("pane_run", std::format(R"({{"pane":"tests","command":"{}"}})", command));
        INFO(command);
        CHECK(out.contains("-32602"));
    }
    CHECK(f.channel.calls.empty());
}

TEST_CASE("pane_run timeoutSeconds is bounded", "[mcp-server]")
{
    auto f = Fixture {};
    scriptRun(f);
    for (auto const* seconds: { "0", "10000001" })
    {
        auto const out = f.call(
            "pane_run", std::format(R"({{"pane":"tests","command":"x","timeoutSeconds":{}}})", seconds));
        INFO(seconds);
        CHECK(out.contains("-32602"));
    }
    CHECK(f.channel.calls.empty());
    CHECK(f.call("pane_run", R"({"pane":"tests","command":"x","timeoutSeconds":10000000})")
              .contains(R"("isError":false)"));
    CHECK(f.channel.last("pane.send")->timeout == std::chrono::seconds { 10'000'000 });
}

TEST_CASE("a request id must be a string or an integer", "[mcp-server]")
{
    auto f = Fixture {};
    for (auto const* id: { "null", "{}", "[1]", "1.5", "true" })
    {
        auto const out = f.server.handleLine(std::format(
            R"({{"jsonrpc":"2.0","id":{},"method":"tools/call","params":{{"name":"pane_type","arguments":{{"pane":"tests","text":"x"}}}}}})",
            id));
        INFO(id);
        REQUIRE(out);
        CHECK(out->contains("-32600"));
        CHECK(out->contains(R"("id":null)"));
    }
    CHECK(f.channel.calls.empty());
    CHECK(f.server.handleLine(R"({"jsonrpc":"2.0","id":"abc","method":"ping"})")->contains(R"("id":"abc")"));
    CHECK(f.server.handleLine(R"({"jsonrpc":"2.0","id":-4,"method":"ping"})")->contains(R"("id":-4)"));
}
