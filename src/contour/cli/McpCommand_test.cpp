// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp`: the stdio loop and the command line around it.

#include <contour/cli/ContourApp.hpp>
#include <contour/cli/ExitStatus.hpp>
#include <contour/cli/McpCommand.hpp>

#include <core/Environment.hpp>
#include <core/async/StopToken.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/Json.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using contour::cli::ExitStatus;
using contour::cli::McpOptions;
using contour::cli::TabScope;
using vthost::client::ControlClientError;

namespace
{

/// A channel that answers every call with `{}`.
class EmptyChannel final: public contour::cli::ControlChannel
{
  public:
    std::expected<std::string, ControlClientError> call(std::string_view,
                                                        std::string_view,
                                                        std::string_view,
                                                        std::optional<std::chrono::milliseconds>) override
    {
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
};

constexpr auto InitializeLine =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"0"}}})"sv;
constexpr auto InitializedLine = R"({"jsonrpc":"2.0","method":"notifications/initialized"})"sv;
constexpr auto ListLine = R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"sv;

std::vector<std::string> linesOf(std::string const& text)
{
    auto lines = std::vector<std::string> {};
    auto stream = std::istringstream { text };
    auto line = std::string {};
    while (std::getline(stream, line))
        lines.push_back(line);
    return lines;
}

int serve(std::string const& input, std::string& output, core::async::StopToken stop = {})
{
    auto channel = EmptyChannel {};
    auto in = std::istringstream { input };
    auto out = std::ostringstream {};
    auto const status = contour::cli::runMcpServer(
        McpOptions { .scope = TabScope::AllTabs }, channel, in, out, std::move(stop));
    output = out.str();
    return status;
}

core::cli::FlagStore parseFlags(std::vector<std::string_view> const& args)
{
    static auto const app = contour::cli::ContourApp { core::defaultEnvironment() };
    auto line = core::cli::StringViewList { "contour" };
    line.insert(line.end(), args.begin(), args.end());
    auto parsed = core::cli::parse(app.parameterDefinition(), line);
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

} // namespace

TEST_CASE("the loop answers each request with one line and none for a notification", "[contour][mcp-command]")
{
    auto output = ""s;
    auto const input = std::string { InitializeLine } + "\n" + std::string { InitializedLine } + "\n\n   \r\n"
                       + std::string { ListLine } + "\n";
    CHECK(serve(input, output) == 0);

    auto const lines = linesOf(output);
    REQUIRE(lines.size() == 2);
    CHECK(output.ends_with("\n"));
    CHECK(lines[0].contains(R"("id":1)"));
    CHECK(lines[0].contains("serverInfo"));
    CHECK(lines[1].contains(R"("id":2)"));
    CHECK(lines[1].contains("pane_capture"));
}

TEST_CASE("the loop ends quietly at end of input", "[contour][mcp-command]")
{
    auto output = "x"s;
    CHECK(serve("", output) == 0);
    CHECK(output.empty());
}

TEST_CASE("the loop answers garbage with an error line, never silence", "[contour][mcp-command]")
{
    auto output = ""s;
    CHECK(serve("not json\n", output) == 0);
    auto const lines = linesOf(output);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].contains("error"));
}

TEST_CASE("the loop stops when asked", "[contour][mcp-command]")
{
    auto source = core::async::StopSource {};
    source.request_stop();
    auto output = ""s;
    CHECK(serve(std::string { ListLine } + "\n", output, source.get_token())
          == contour::cli::exitCode(ExitStatus::Interrupted));
    CHECK(output.empty());
}

TEST_CASE("the options map the flags and the environment", "[contour][mcp-command]")
{
    auto const pane = "pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad"s;
    auto const env = core::testing::FakeEnvironment { { { "CONTOUR_PANE", pane } } };

    auto const own = contour::cli::makeMcpOptions(parseFlags({ "mcp" }), env, "9.9");
    REQUIRE(own.has_value());
    CHECK(own->scope == TabScope::OwnTab);
    CHECK(own->access == contour::cli::McpAccess::Full);
    CHECK(own->callerPane == pane);
    CHECK(own->serverVersion == "9.9");

    auto const wide =
        contour::cli::makeMcpOptions(parseFlags({ "mcp", "--allow-all-tabs", "--read-only" }), env, "1");
    REQUIRE(wide.has_value());
    CHECK(wide->scope == TabScope::AllTabs);
    CHECK(wide->access == contour::cli::McpAccess::ReadOnly);
}

TEST_CASE("without a caller pane the command needs --allow-all-tabs", "[contour][mcp-command]")
{
    auto const env = core::testing::FakeEnvironment {};
    CHECK(!contour::cli::makeMcpOptions(parseFlags({ "mcp" }), env, "1").has_value());
    CHECK(contour::cli::makeMcpOptions(parseFlags({ "mcp", "--allow-all-tabs" }), env, "1").has_value());

    auto fs = core::platform::testing::InMemoryFileSystem {};
    auto in = std::istringstream {};
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    auto const clock = core::platform::ManualWallClock {};
    auto const io =
        contour::cli::ControlIo { .env = env, .fs = fs, .in = in, .out = out, .err = err, .clock = clock };
    CHECK(contour::cli::runMcpCommand(parseFlags({ "mcp" }), io, "1")
          == contour::cli::exitCode(ExitStatus::BadUsage));
    CHECK(err.str().contains("CONTOUR_PANE"));
    CHECK(out.str().empty());
}
