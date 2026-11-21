// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// What an agent run cost and did, as `contour agent stats --json` prints it (format 1,
/// contract/agent-stats/): read into the run and stored with it, so that it outlives both the run's worktree
/// and Claude Code's log. Its members are the format's keys, so the report is read and stored in one shape.
namespace orchestrator
{

/// The format of `contour agent stats --json` this core reads.
inline constexpr uint32_t StatsFormat = 1;

/// The most tools a stored report keeps; the verb names at most as many.
inline constexpr std::size_t MaxStoredTools = 16;

/// The most pull requests a stored report keeps.
inline constexpr std::size_t MaxStoredPullRequests = 8;

/// The most bytes of a text a stored report keeps: a tool's name, a time, a version, a URL.
inline constexpr std::size_t MaxStoredStatsText = 256;

/// Tokens, by kind.
struct StatsTokens
{
    uint64_t input = 0;      ///< Input tokens not read from or written to the cache.
    uint64_t output = 0;     ///< Output tokens, thinking included.
    uint64_t thinking = 0;   ///< The output tokens spent thinking.
    uint64_t cacheWrite = 0; ///< Input tokens written to the cache.
    uint64_t cacheRead = 0;  ///< Input tokens read from the cache.

    [[nodiscard]] bool operator==(StatsTokens const&) const = default;
};

/// How often the agent called one tool.
struct StatsTool
{
    std::string name;
    uint64_t calls = 0;

    [[nodiscard]] bool operator==(StatsTool const&) const = default;
};

/// What the agent's subagents spent.
struct StatsSubagents
{
    uint32_t count = 0;
    uint64_t apiCalls = 0;
    StatsTokens tokens;

    [[nodiscard]] bool operator==(StatsSubagents const&) const = default;
};

/// A pull request the agent opened.
struct StatsPullRequest
{
    uint64_t number = 0;
    std::string url;

    [[nodiscard]] bool operator==(StatsPullRequest const&) const = default;
};

/// What Claude Code recorded when the agent's session exited.
struct StatsCost
{
    double usd = 0;                   ///< As Claude Code computed it, subagents included.
    uint64_t apiMs = 0;               ///< Time in API calls, retries included.
    uint64_t apiMsWithoutRetries = 0; ///< The same, without retries.
    uint64_t toolMs = 0;              ///< Time running tools.
    uint64_t totalMs = 0;             ///< The session's whole duration.
    uint64_t linesAdded = 0;
    uint64_t linesRemoved = 0;

    [[nodiscard]] bool operator==(StatsCost const&) const = default;
};

/// What an agent run cost and did.
struct RunStats
{
    std::string writtenBy;                      ///< The Claude Code version of the log's last entry.
    std::string first;                          ///< The log's first timestamp, ISO 8601.
    std::string last;                           ///< Its last.
    StatsTokens tokens;                         ///< The main conversation's tokens.
    uint64_t apiCalls = 0;                      ///< Its API responses.
    uint64_t apiErrors = 0;                     ///< API calls that failed.
    uint64_t prompts = 0;                       ///< Prompts the user typed, the first included.
    uint64_t activeMs = 0;                      ///< Time Claude Code measured working on turns.
    uint64_t toolCalls = 0;                     ///< Every tool call.
    uint64_t toolFailures = 0;                  ///< Tool results marked as errors.
    std::vector<StatsTool> tools;               ///< The most called tools, most first.
    uint64_t compactions = 0;                   ///< Times the conversation was compacted.
    StatsSubagents subagents;                   ///< What its subagents spent.
    std::vector<StatsPullRequest> pullRequests; ///< The pull requests it opened.
    std::optional<StatsCost> cost;              ///< What Claude Code recorded on exit; none while it runs.
    uint64_t notUnderstood = 0;                 ///< Log lines the verb could not read.

    [[nodiscard]] bool operator==(RunStats const&) const = default;
};

/// Why a report could not be read.
enum class StatsReadError : uint8_t
{
    FormatMismatch, ///< Another format, or contour refused the one asked for: one side needs updating.
    NotStats,       ///< The output is no report.
};

/// Reads a report, bounding what it keeps (MaxStoredTools, MaxStoredPullRequests, MaxStoredStatsText).
/// @param json `contour agent stats --json`'s output; untrusted.
/// @return The report; FormatMismatch for another format, NotStats for anything else that is not one.
[[nodiscard]] std::expected<RunStats, StatsReadError> parseStats(std::string_view json);

/// @param stderrText What a failed `contour agent stats` printed.
/// @return FormatMismatch when contour refused the format asked for; NotStats otherwise.
[[nodiscard]] StatsReadError classifyStatsFailure(std::string_view stderrText) noexcept;

/// @param tokens Tokens.
/// @return The input tokens among them, cached or not.
[[nodiscard]] uint64_t inputTokensOf(StatsTokens const& tokens) noexcept;

/// @param tokens Tokens.
/// @return The share of input tokens read from the cache, 0 to 1; nothing when there was no input.
[[nodiscard]] std::optional<double> cacheHitOf(StatsTokens const& tokens) noexcept;

} // namespace orchestrator
