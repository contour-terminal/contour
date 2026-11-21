// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Stats.hpp>

#include <string_view>
#include <utility>

#include <glaze/glaze.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/Sanitize.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    /// The input is a string_view: nothing terminates it, and a newer `contour` may add keys.
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// What contour prints on standard error when it refuses the format asked for.
    constexpr auto UnsupportedFormatPrefix = "unsupported stats format "sv;

    /// @return @p text as a stored report keeps it: drawable as it is, and bounded.
    [[nodiscard]] std::string bounded(std::string_view text)
    {
        return displayText(text, MaxStoredStatsText);
    }
} // namespace

} // namespace orchestrator

// Glaze reflects only types with linkage: the format's probe lives here, in a named namespace.
namespace orchestrator::stats::detail
{
/// What is read of a report first: its format, which decides whether the rest may be read at all.
struct JsonFormat
{
    uint32_t format = 0;
};
} // namespace orchestrator::stats::detail

namespace orchestrator
{

std::expected<RunStats, StatsReadError> parseStats(std::string_view json)
{
    // The format first: a report of another format may have another shape, which is a mismatch, not a fault.
    auto const format = json::readDocument<ReadOptions, stats::detail::JsonFormat>(json);
    if (!format)
        return std::unexpected(StatsReadError::NotStats);
    if (format->format != StatsFormat)
        return std::unexpected(StatsReadError::FormatMismatch);
    auto read = json::readDocument<ReadOptions, RunStats>(json);
    if (!read)
        return std::unexpected(StatsReadError::NotStats);
    auto stats = std::move(*read);
    if (stats.tools.size() > MaxStoredTools)
        stats.tools.resize(MaxStoredTools);
    if (stats.pullRequests.size() > MaxStoredPullRequests)
        stats.pullRequests.resize(MaxStoredPullRequests);
    stats.writtenBy = bounded(stats.writtenBy);
    stats.first = bounded(stats.first);
    stats.last = bounded(stats.last);
    for (auto& tool: stats.tools)
        tool.name = bounded(tool.name);
    for (auto& pullRequest: stats.pullRequests)
        pullRequest.url = bounded(pullRequest.url);
    return stats;
}

StatsReadError classifyStatsFailure(std::string_view stderrText) noexcept
{
    return stderrText.starts_with(UnsupportedFormatPrefix) ? StatsReadError::FormatMismatch
                                                           : StatsReadError::NotStats;
}

uint64_t inputTokensOf(StatsTokens const& tokens) noexcept
{
    return tokens.input + tokens.cacheWrite + tokens.cacheRead;
}

std::optional<double> cacheHitOf(StatsTokens const& tokens) noexcept
{
    auto const input = inputTokensOf(tokens);
    if (input == 0)
        return std::nullopt;
    return static_cast<double>(tokens.cacheRead) / static_cast<double>(input);
}

} // namespace orchestrator
