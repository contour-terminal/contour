// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentStats.hpp>
#include <contour/cli/ExitStatus.hpp>
#include <contour/cli/SessionLogTestSupport.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;
using namespace contour::cli;
using namespace contour::cli::testing;

namespace
{

[[nodiscard]] std::string readFile(std::filesystem::path const& path)
{
    auto in = std::ifstream { path, std::ios::binary };
    REQUIRE(in.good());
    return std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
}

/// @return The synthetic log @p name.
[[nodiscard]] std::string fixture(std::string_view name)
{
    return readFile(std::filesystem::path { CONTOUR_AGENT_STATS_TESTDATA } / "synthetic" / name);
}

/// @return One assistant line of message @p id with the given usage, calling @p tool when not empty.
[[nodiscard]] std::string assistantLine(std::string_view id,
                                        uint64_t output,
                                        std::string_view tool = {},
                                        std::string_view extra = {})
{
    auto const content =
        tool.empty() ? std::string { R"([{"type":"text","text":"x"}])" }
                     : std::format(R"([{{"type":"tool_use","id":"t","name":"{}","input":{{}}}}])", tool);
    return std::format(R"({{"type":"assistant","message":{{"id":"{}","content":{},)"
                       R"("usage":{{"input_tokens":1,"output_tokens":{},"cache_read_input_tokens":10}}}},)"
                       R"({}"cwd":"/home/user/proj"}})",
                       id,
                       content,
                       output,
                       extra);
}

struct Outcome
{
    int status = 0;
    std::string out;
    std::string err;
};

/// A home with Claude Code's projects directory, the session's log in it, and its subagents' logs.
struct World
{
    core::platform::testing::InMemoryFileSystem fs;
    MemoryOpener opener;
    core::testing::FakeEnvironment env { std::map<std::string, std::string, std::less<>> {
        { std::string { ClaudeHomeVariable }, nativePath(Home) } } };

    World() { fs.addDirectory(projectsDirectory()); }

    /// Adds the session's log under `<projects>/proj/`.
    std::filesystem::path addLog(std::string_view content)
    {
        auto const directory = projectsDirectory() / "proj";
        fs.addDirectory(directory);
        auto const log = directory / std::format("{}.jsonl", Session);
        fs.addFile(log, "in the opener");
        opener.add(log, withNativeCwds(content));
        return log;
    }

    /// Adds a subagent's log `agent-<name>.jsonl` beside the session's.
    void addSubagent(std::string_view name, std::string_view content)
    {
        auto const directory = projectsDirectory() / "proj" / Session / "subagents";
        fs.addDirectory(projectsDirectory() / "proj" / Session);
        fs.addDirectory(directory);
        auto const log = directory / std::format("agent-{}.jsonl", name);
        fs.addFile(log, "in the opener");
        opener.add(log, withNativeCwds(content));
    }

    [[nodiscard]] Outcome run(StatsOutput output = StatsOutput::Json, uint32_t format = StatsFormat)
    {
        auto out = std::ostringstream {};
        auto err = std::ostringstream {};
        auto const request = StatsRequest { .session = std::string { Session },
                                            .cwd = nativePath(Worktree),
                                            .format = format,
                                            .output = output };
        auto const status = runAgentStats(request, env, fs, opener, out, err);
        return Outcome { .status = status, .out = out.str(), .err = err.str() };
    }
};

/// @return What the reader makes of @p log's lines.
[[nodiscard]] SessionStats statsOf(std::string_view log)
{
    auto reader = SessionStatsReader {};
    auto in = std::istringstream { std::string { log } };
    auto lines = detail::LineReader { in };
    while (auto const line = lines.next())
        reader.read(*line);
    return std::move(reader).finish();
}

} // namespace

TEST_CASE("the verb prints the contract's reports", "[cli][agent][stats][contract]")
{
    struct Report
    {
        std::string_view log;    ///< The fixture it was printed from, as the contract's README names it.
        std::string_view report; ///< The report in `format-1/`.
    };
    constexpr auto Reports = std::array {
        Report { .log = "basic.jsonl", .report = "basic.json" },
        Report { .log = "running.jsonl", .report = "running.json" },
        Report { .log = "empty.jsonl", .report = "empty.json" },
    };
    for (auto const& row: Reports)
    {
        CAPTURE(row.log);
        auto world = World {};
        world.addLog(fixture(row.log));
        auto const outcome = world.run();
        INFO(outcome.err);
        REQUIRE(outcome.status == 0);
        CHECK(outcome.out
              == readFile(std::filesystem::path { CONTOUR_AGENT_STATS_CONTRACT } / "format-1" / row.report));
    }
}

TEST_CASE("a response written as several lines counts its tokens once", "[cli][agent][stats]")
{
    auto const log = std::format("{}\n{}\n{}\n",
                                 assistantLine("m1", 100, "Bash"),
                                 assistantLine("m1", 100, "Read"),
                                 assistantLine("m2", 5));
    auto const stats = statsOf(log);
    CHECK(stats.apiCalls == 2);
    CHECK(stats.tokens
          == TokenCounts { .input = 2, .output = 105, .thinking = 0, .cacheWrite = 0, .cacheRead = 20 });
    // Each line's block is its own: both tool calls count.
    CHECK(stats.toolCalls == 2);
}

TEST_CASE("the tools are the most called first, at most sixteen", "[cli][agent][stats]")
{
    auto log = std::string {};
    for (auto const tool: std::views::iota(0, 20))
        for ([[maybe_unused]] auto const call: std::views::iota(0, tool + 1))
            log += assistantLine(std::format("m{}-{}", tool, call), 1, std::format("T{:02}", tool)) + '\n';
    log += assistantLine("tie", 1, "A") + '\n';
    auto const stats = statsOf(log);
    REQUIRE(stats.tools.size() == MaxStatsTools);
    CHECK(stats.tools.front() == ToolCount { .name = "T19", .calls = 20 });
    CHECK(stats.tools.back() == ToolCount { .name = "T04", .calls = 5 });
    // Those left out still count.
    CHECK(stats.toolCalls == 211);
}

TEST_CASE("a tool's name and a pull request's URL are sanitised", "[cli][agent][stats]")
{
    auto const log = std::format(
        "{}\n{}\n",
        assistantLine("m1", 1, R"(Bash\u001b[31m)"),
        R"({"type":"pr-link","prNumber":1,"prUrl":"https://x/‮pull/1","cwd":"/home/user/proj"})");
    auto const stats = statsOf(log);
    REQUIRE(stats.tools.size() == 1);
    CHECK_FALSE(stats.tools.front().name.contains('\x1b'));
    REQUIRE(stats.pullRequests.size() == 1);
    CHECK_FALSE(stats.pullRequests.front().url.contains("‮"));
}

TEST_CASE("a line that does not parse is counted, never fatal", "[cli][agent][stats]")
{
    auto const stats = statsOf(fixture("broken.jsonl"));
    CHECK(stats.notUnderstood == 1);
    CHECK(stats.prompts == 1);
    CHECK(stats.apiCalls == 1);
}

TEST_CASE("the last cost record covers a resumed session", "[cli][agent][stats]")
{
    auto const log = std::string { R"({"type":"cost-state","totalCostUSD":1.0,"totalDuration":10})"
                                   "\n"
                                   R"({"type":"cost-state","totalCostUSD":2.5,"totalDuration":30})"
                                   "\n" };
    auto const stats = statsOf(log);
    REQUIRE(stats.cost.has_value());
    CHECK(stats.cost->usd == 2.5);
    CHECK(stats.cost->totalMs == 30);
}

TEST_CASE("subagents' logs beside the session's are read", "[cli][agent][stats]")
{
    auto world = World {};
    world.addLog(fixture("basic.jsonl"));
    world.addSubagent("a", std::format("{}\n{}\n", assistantLine("s1", 7), assistantLine("s1", 7)));
    world.addSubagent("b", assistantLine("s2", 3) + '\n');
    // Written elsewhere: not the run's.
    world.addSubagent(
        "c",
        R"({"type":"assistant","message":{"id":"s3","content":[],"usage":{"output_tokens":99}},)"
        R"("cwd":"/elsewhere"})"
        "\n");
    auto const outcome = world.run();
    REQUIRE(outcome.status == 0);
    CHECK(outcome.out.contains(
        R"("subagents":{"count":2,"apiCalls":2,"tokens":{"input":2,"output":10,"thinking":0,"cacheWrite":0,"cacheRead":20}})"));
}

TEST_CASE("a sidechain in the session's own log is a subagent's", "[cli][agent][stats]")
{
    auto const stats = statsOf(assistantLine("m1", 4, {}, R"("isSidechain":true,)") + '\n');
    CHECK(stats.apiCalls == 0);
    CHECK(stats.subagents.apiCalls == 1);
    CHECK(stats.subagents.tokens.output == 4);
}

TEST_CASE("another format is refused, by the line the orchestrator knows", "[cli][agent][stats]")
{
    auto world = World {};
    world.addLog(fixture("basic.jsonl"));
    auto const outcome = world.run(StatsOutput::Json, 2);
    CHECK(outcome.status == exitCode(ExitStatus::BadUsage));
    CHECK(outcome.err == "unsupported stats format 2; this contour prints 1\n");
    CHECK(outcome.out.empty());
}

TEST_CASE("a session with no log of the run is refused", "[cli][agent][stats]")
{
    auto world = World {};
    world.addLog(R"({"type":"user","cwd":"/elsewhere"})"
                 "\n");
    auto const outcome = world.run();
    CHECK(outcome.status == exitCode(ExitStatus::TargetNotFound));
    CHECK(outcome.err.starts_with("no session log for "));
}

TEST_CASE("the text report names what a person reads", "[cli][agent][stats]")
{
    auto world = World {};
    world.addLog(fixture("basic.jsonl"));
    auto const finished = world.run(StatsOutput::Text);
    REQUIRE(finished.status == 0);
    CHECK(finished.out.contains("cost      $0.12 as Claude Code reports it"));
    CHECK(finished.out.contains("tools     3 calls: Bash 2 · Read 1 · 1 failed"));
    CHECK(finished.out.contains("opened    #7 https://github.com/o/r/pull/7"));

    auto running = World {};
    running.addLog(fixture("running.jsonl"));
    CHECK(running.run(StatsOutput::Text).out.contains("cost      not recorded yet"));
}
