// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp` server core: the JSON-RPC envelope, the scope rules and the catalog tools, over a channel
// that answers from a script and records what it was asked.

#include <contour/cli/McpServer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <map>
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
    };

    std::expected<std::string, ControlClientError> call(
        std::string_view method,
        std::string_view params,
        std::string_view context,
        std::optional<std::chrono::milliseconds> /*t*/) override
    {
        calls.push_back({ std::string(method), std::string(params), std::string(context) });
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
        for (auto it = calls.rbegin(); it != calls.rend(); ++it)
            if (it->method == method)
                return &*it;
        return nullptr;
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
