// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The shapes `contour agent stats` reads from a session log and writes as format 1; only AgentStats.cpp
/// includes this header. Glaze reflects only types with linkage, so these cannot sit in that file's anonymous
/// namespace. The log's keys are Claude Code's, mapped by name below.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>

namespace contour::cli::detail
{

/// `message.usage.output_tokens_details`.
struct StatsOutputDetails
{
    std::optional<uint64_t> thinkingTokens;
};

/// `message.usage`: one API response's tokens.
struct StatsUsage
{
    std::optional<uint64_t> inputTokens;
    std::optional<uint64_t> outputTokens;
    std::optional<uint64_t> cacheCreationInputTokens;
    std::optional<uint64_t> cacheReadInputTokens;
    std::optional<StatsOutputDetails> outputTokensDetails;
};

/// One block of a message's content; its text, input and result are skipped unread.
struct StatsBlock
{
    std::string type;
    std::optional<std::string> name;
    std::optional<bool> isError;
};

/// A line's `message`.
struct StatsMessage
{
    std::optional<std::string> id;
    std::optional<StatsUsage> usage;
    std::optional<std::variant<std::string, std::vector<StatsBlock>>> content;
};

/// What a line is read into; every other key is skipped.
struct StatsLine
{
    std::string type;
    std::optional<std::string> subtype;
    std::optional<std::string> timestamp;
    std::optional<std::string> version;
    std::optional<std::string> promptId;
    std::optional<bool> isMeta;
    std::optional<bool> isSidechain;
    std::optional<bool> isApiErrorMessage;
    std::optional<StatsMessage> message;
    std::optional<uint64_t> durationMs; ///< `turn_duration`: the turn's working time.
    std::optional<uint64_t> prNumber;   ///< `pr-link`.
    std::optional<std::string> prUrl;   ///< `pr-link`.
    std::optional<double> totalCostUsd; ///< `cost-state`, as are the rest.
    std::optional<uint64_t> totalApiMs;
    std::optional<uint64_t> totalApiMsWithoutRetries;
    std::optional<uint64_t> totalToolMs;
    std::optional<uint64_t> totalMs;
    std::optional<uint64_t> totalLinesAdded;
    std::optional<uint64_t> totalLinesRemoved;
};

/// Format 1's token counts.
struct WireTokens
{
    uint64_t input = 0;
    uint64_t output = 0;
    uint64_t thinking = 0;
    uint64_t cacheWrite = 0;
    uint64_t cacheRead = 0;
};

struct WireTool
{
    std::string_view name;
    uint64_t calls = 0;
};

struct WireSubagents
{
    uint32_t count = 0;
    uint64_t apiCalls = 0;
    WireTokens tokens;
};

struct WirePullRequest
{
    uint64_t number = 0;
    std::string_view url;
};

struct WireCost
{
    double usd = 0;
    uint64_t apiMs = 0;
    uint64_t apiMsWithoutRetries = 0;
    uint64_t toolMs = 0;
    uint64_t totalMs = 0;
    uint64_t linesAdded = 0;
    uint64_t linesRemoved = 0;
};

/// Format 1, its keys in their contract order.
struct WireStats
{
    uint32_t format = 1;
    std::string_view session;
    std::string_view writtenBy;
    std::string_view first;
    std::string_view last;
    WireTokens tokens;
    uint64_t apiCalls = 0;
    uint64_t apiErrors = 0;
    uint64_t prompts = 0;
    uint64_t activeMs = 0;
    uint64_t toolCalls = 0;
    uint64_t toolFailures = 0;
    std::vector<WireTool> tools;
    uint64_t compactions = 0;
    WireSubagents subagents;
    std::vector<WirePullRequest> pullRequests;
    std::optional<WireCost> cost; ///< null while the session runs.
    std::size_t notUnderstood = 0;
};

} // namespace contour::cli::detail

// Glaze finds each table by this spelling, as the standard library finds `value_type`.
template <>
struct glz::meta<contour::cli::detail::StatsOutputDetails>
{
    using T = contour::cli::detail::StatsOutputDetails;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("thinking_tokens", &T::thinkingTokens);
};

template <>
struct glz::meta<contour::cli::detail::StatsUsage>
{
    using T = contour::cli::detail::StatsUsage;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("input_tokens",
                                              &T::inputTokens,
                                              "output_tokens",
                                              &T::outputTokens,
                                              "cache_creation_input_tokens",
                                              &T::cacheCreationInputTokens,
                                              "cache_read_input_tokens",
                                              &T::cacheReadInputTokens,
                                              "output_tokens_details",
                                              &T::outputTokensDetails);
};

template <>
struct glz::meta<contour::cli::detail::StatsBlock>
{
    using T = contour::cli::detail::StatsBlock;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("type", &T::type, "name", &T::name, "is_error", &T::isError);
};

template <>
struct glz::meta<contour::cli::detail::StatsLine>
{
    using T = contour::cli::detail::StatsLine;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("type",
                                              &T::type,
                                              "subtype",
                                              &T::subtype,
                                              "timestamp",
                                              &T::timestamp,
                                              "version",
                                              &T::version,
                                              "promptId",
                                              &T::promptId,
                                              "isMeta",
                                              &T::isMeta,
                                              "isSidechain",
                                              &T::isSidechain,
                                              "isApiErrorMessage",
                                              &T::isApiErrorMessage,
                                              "message",
                                              &T::message,
                                              "durationMs",
                                              &T::durationMs,
                                              "prNumber",
                                              &T::prNumber,
                                              "prUrl",
                                              &T::prUrl,
                                              "totalCostUSD",
                                              &T::totalCostUsd,
                                              "totalAPIDuration",
                                              &T::totalApiMs,
                                              "totalAPIDurationWithoutRetries",
                                              &T::totalApiMsWithoutRetries,
                                              "totalToolDuration",
                                              &T::totalToolMs,
                                              "totalDuration",
                                              &T::totalMs,
                                              "totalLinesAdded",
                                              &T::totalLinesAdded,
                                              "totalLinesRemoved",
                                              &T::totalLinesRemoved);
};
