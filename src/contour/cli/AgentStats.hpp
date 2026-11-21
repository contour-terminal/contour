// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `contour agent stats`: what a Claude Code session cost and did -- its tokens, API calls, tool calls,
/// turns, compactions, subagents, the pull requests it opened, and the cost Claude Code recorded when it
/// exited -- read from its log and printed as text or as the versioned JSON the agent orchestrator reads
/// (format 1, `extensions/orchestrator/contract/agent-stats/`).
///
/// The log is found, bound to the run and read as `contour agent transcript` reads it (AgentSessionLog.hpp).
/// One API response is written as several lines that repeat its usage, so tokens are counted once per
/// message id. A line no rule reads is skipped; one that does not parse is counted, never fatal.

#include <contour/cli/AgentSessionLog.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace contour::cli
{

/// The output format this contour prints; another one asked for is refused.
inline constexpr uint32_t StatsFormat = 1;

/// The most tools a report names, the most called first; the calls of the rest still count in the totals.
inline constexpr std::size_t MaxStatsTools = 16;

/// The most pull requests a report names.
inline constexpr std::size_t MaxStatsPullRequests = 8;

/// The most bytes of a tool's name, a time or a version kept.
inline constexpr std::size_t MaxStatsFieldBytes = 64;

/// The most bytes of a pull request's URL kept.
inline constexpr std::size_t MaxStatsUrlBytes = 256;

/// Tokens, by kind.
struct TokenCounts
{
    uint64_t input = 0;      ///< Input tokens not read from or written to the cache.
    uint64_t output = 0;     ///< Output tokens, thinking included.
    uint64_t thinking = 0;   ///< The output tokens spent thinking.
    uint64_t cacheWrite = 0; ///< Input tokens written to the cache.
    uint64_t cacheRead = 0;  ///< Input tokens read from the cache.

    [[nodiscard]] bool operator==(TokenCounts const&) const = default;
};

/// How often a session called one tool.
struct ToolCount
{
    std::string name; ///< The tool's name, sanitised.
    uint64_t calls = 0;

    [[nodiscard]] bool operator==(ToolCount const&) const = default;
};

/// A pull request the session opened.
struct PullRequestLink
{
    uint64_t number = 0;
    std::string url; ///< Sanitised.

    [[nodiscard]] bool operator==(PullRequestLink const&) const = default;
};

/// What a session's subagents spent, each read from its own log beside the session's.
struct SubagentStats
{
    uint32_t count = 0;    ///< The subagent logs read.
    uint64_t apiCalls = 0; ///< Their API calls.
    TokenCounts tokens;    ///< Their tokens.

    [[nodiscard]] bool operator==(SubagentStats const&) const = default;
};

/// What Claude Code recorded when the session exited: its last `cost-state` line.
struct SessionCost
{
    double usd = 0;                   ///< The cost Claude Code computed, subagents and side calls included.
    uint64_t apiMs = 0;               ///< Time spent in API calls, retries included.
    uint64_t apiMsWithoutRetries = 0; ///< The same, without retries.
    uint64_t toolMs = 0;              ///< Time spent running tools.
    uint64_t totalMs = 0;             ///< The session's whole duration.
    uint64_t linesAdded = 0;          ///< Lines its edits added.
    uint64_t linesRemoved = 0;        ///< Lines its edits removed.

    [[nodiscard]] bool operator==(SessionCost const&) const = default;
};

/// What a session cost and did.
struct SessionStats
{
    std::string writtenBy;     ///< The Claude Code version of its last entry; sanitised.
    std::string firstTime;     ///< Its first entry's ISO 8601 timestamp; sanitised; empty when none has one.
    std::string lastTime;      ///< Its last entry's.
    TokenCounts tokens;        ///< The main conversation's tokens, each API response counted once.
    uint64_t apiCalls = 0;     ///< Its API responses: distinct message ids.
    uint64_t apiErrors = 0;    ///< API calls that ended in an error Claude Code recorded.
    uint64_t prompts = 0;      ///< The user's prompts: the first, and every answer after it.
    uint64_t activeMs = 0;     ///< Time spent working on turns, as Claude Code measured them.
    uint64_t toolCalls = 0;    ///< Every tool call.
    uint64_t toolFailures = 0; ///< Tool results marked as errors.
    std::vector<ToolCount> tools;              ///< The most called tools, most first; at most MaxStatsTools.
    uint64_t compactions = 0;                  ///< Times the conversation was compacted.
    SubagentStats subagents;                   ///< What its subagents spent.
    std::vector<PullRequestLink> pullRequests; ///< The pull requests it opened; at most MaxStatsPullRequests.
    std::optional<SessionCost> cost; ///< What Claude Code recorded on exit; none while the session runs.
    std::size_t notUnderstood = 0;   ///< Lines that did not parse, or were cut at MaxLogLineBytes.

    [[nodiscard]] bool operator==(SessionStats const&) const = default;
};

/// How a report is printed.
enum class StatsOutput : uint8_t
{
    Text = 0, ///< For a person.
    Json,     ///< Format StatsFormat, for the orchestrator.
};

/// What `contour agent stats` was asked.
struct StatsRequest
{
    std::string session;                    ///< The session UUID.
    std::string cwd;                        ///< The run's directory; every entry's must lie beneath it.
    uint32_t format = StatsFormat;          ///< The output format asked for.
    StatsOutput output = StatsOutput::Text; ///< How to print it.
};

/// Reads a session's statistics one log line at a time: the session's own log, then each subagent's.
class SessionStatsReader
{
  public:
    /// Takes in a line of the session's own log.
    void read(detail::LogLine const& line);

    /// Takes in a line of one subagent's log; call beginSubagent() before its first.
    void readSubagent(detail::LogLine const& line);

    /// Starts a subagent's log.
    void beginSubagent();

    /// @return What was read.
    [[nodiscard]] SessionStats finish() &&;

  private:
    SessionStats _stats;
    std::vector<std::string> _messages;         ///< The main conversation's message ids seen, sorted.
    std::vector<std::string> _subagentMessages; ///< The current subagent's, sorted.
    std::vector<ToolCount> _tools;              ///< Every tool called, unsorted.
};

/// Runs `contour agent stats`: finds the session's log as `contour agent transcript` does, reads it and its
/// subagents' logs (`<session>/subagents/agent-*.jsonl` beside it), and prints what they add up to.
/// @param request What was asked.
/// @param env Where `$CLAUDE_CONFIG_DIR` and ClaudeHomeVariable are read.
/// @param fs Where the projects directory and the subagents' directory are listed.
/// @param opener What opens each log.
/// @param out Where the report is printed.
/// @param err Where the one line of a refusal is printed.
/// @return The ExitStatus as an exit code.
[[nodiscard]] int runAgentStats(StatsRequest const& request,
                                core::Environment const& env,
                                core::platform::FileSystem const& fs,
                                ISessionLogOpener& opener,
                                std::ostream& out,
                                std::ostream& err);

} // namespace contour::cli
