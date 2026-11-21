// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentStats.hpp>
#include <contour/cli/AgentStatsJson.hpp>
#include <contour/cli/ExitStatus.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include <glaze/glaze.hpp>
#include <vthost/control/StatusText.hpp>

using namespace std::string_view_literals;

namespace contour::cli
{

namespace
{
    using detail::LineExtent;
    using detail::LogLine;
    using detail::StatsBlock;
    using detail::StatsLine;
    using detail::StatsUsage;

    /// Unknown keys skipped, nothing past the line read.
    constexpr auto LineReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// Format 1 writes a session still running as `"cost":null`, never by leaving the key out.
    constexpr auto WireOptions = glz::opts { .skip_null_members = false };

    /// Where a subagent's log lies, beside the session's: `<project>/<session>/subagents/agent-*.jsonl`.
    constexpr auto SubagentDirectory = "subagents"sv;
    constexpr auto SubagentPrefix = "agent-"sv;
    constexpr auto LogExtension = ".jsonl"sv;

    /// @return @p text sanitised to at most @p maxBytes.
    [[nodiscard]] std::string sanitized(std::string_view text, std::size_t maxBytes)
    {
        return vthost::control::sanitizeStatusText(text, maxBytes);
    }

    void addUsage(TokenCounts& tokens, StatsUsage const& usage) noexcept
    {
        tokens.input += usage.inputTokens.value_or(0);
        tokens.output += usage.outputTokens.value_or(0);
        tokens.cacheWrite += usage.cacheCreationInputTokens.value_or(0);
        tokens.cacheRead += usage.cacheReadInputTokens.value_or(0);
        if (usage.outputTokensDetails)
            tokens.thinking += usage.outputTokensDetails->thinkingTokens.value_or(0);
    }

    /// @return Whether @p id was not in @p seen, a sorted list it is then added to.
    [[nodiscard]] bool firstSighting(std::vector<std::string>& seen, std::string const& id)
    {
        auto const at = std::ranges::lower_bound(seen, id);
        if (at != seen.end() && *at == id)
            return false;
        seen.insert(at, id);
        return true;
    }

    /// @return The blocks of @p line's message; empty when its content is text, or it has none.
    [[nodiscard]] std::span<StatsBlock const> blocksOf(StatsLine const& line) noexcept
    {
        if (!line.message || !line.message->content)
            return {};
        auto const* blocks = std::get_if<std::vector<StatsBlock>>(&*line.message->content);
        return blocks != nullptr ? std::span<StatsBlock const> { *blocks } : std::span<StatsBlock const> {};
    }

    /// @return Whether @p line is a prompt the user typed: not a tool's result, nor text Claude Code
    /// inserted.
    [[nodiscard]] bool isPrompt(StatsLine const& line) noexcept
    {
        if (!line.promptId || line.isMeta.value_or(false) || line.isSidechain.value_or(false))
            return false;
        return std::ranges::none_of(blocksOf(line),
                                    [](StatsBlock const& block) { return block.type == "tool_result"; });
    }

    /// @return @p line parsed; nothing for a blank line; std::unexpected for one cut or not understood.
    [[nodiscard]] std::expected<std::optional<StatsLine>, std::monostate> parsed(LogLine const& line)
    {
        if (line.text.find_first_not_of(" \t\r") == std::string_view::npos)
            return std::nullopt;
        auto fields = StatsLine {};
        if (line.extent == LineExtent::Cut || glz::read<LineReadOptions>(fields, line.text))
            return std::unexpected(std::monostate {});
        return std::optional { std::move(fields) };
    }

    /// @return @p usd rounded to a millionth of a dollar, so that format 1 prints the same bytes for the same
    ///         log whatever sum of floating-point costs Claude Code wrote.
    [[nodiscard]] double roundedUsd(double usd) noexcept
    {
        return std::round(usd * 1'000'000) / 1'000'000;
    }

    // {{{ printing

    [[nodiscard]] detail::WireTokens wireTokens(TokenCounts const& tokens) noexcept
    {
        return detail::WireTokens { .input = tokens.input,
                                    .output = tokens.output,
                                    .thinking = tokens.thinking,
                                    .cacheWrite = tokens.cacheWrite,
                                    .cacheRead = tokens.cacheRead };
    }

    [[nodiscard]] std::string jsonStats(SessionStats const& stats, std::string_view session)
    {
        auto wire = detail::WireStats { .format = StatsFormat,
                                        .session = session,
                                        .writtenBy = stats.writtenBy,
                                        .first = stats.firstTime,
                                        .last = stats.lastTime,
                                        .tokens = wireTokens(stats.tokens),
                                        .apiCalls = stats.apiCalls,
                                        .apiErrors = stats.apiErrors,
                                        .prompts = stats.prompts,
                                        .activeMs = stats.activeMs,
                                        .toolCalls = stats.toolCalls,
                                        .toolFailures = stats.toolFailures,
                                        .tools = {},
                                        .compactions = stats.compactions,
                                        .subagents = { .count = stats.subagents.count,
                                                       .apiCalls = stats.subagents.apiCalls,
                                                       .tokens = wireTokens(stats.subagents.tokens) },
                                        .pullRequests = {},
                                        .cost = std::nullopt,
                                        .notUnderstood = stats.notUnderstood };
        for (auto const& tool: stats.tools)
            wire.tools.push_back(detail::WireTool { .name = tool.name, .calls = tool.calls });
        for (auto const& pullRequest: stats.pullRequests)
            wire.pullRequests.push_back(
                detail::WirePullRequest { .number = pullRequest.number, .url = pullRequest.url });
        if (stats.cost)
            wire.cost = detail::WireCost { .usd = stats.cost->usd,
                                           .apiMs = stats.cost->apiMs,
                                           .apiMsWithoutRetries = stats.cost->apiMsWithoutRetries,
                                           .toolMs = stats.cost->toolMs,
                                           .totalMs = stats.cost->totalMs,
                                           .linesAdded = stats.cost->linesAdded,
                                           .linesRemoved = stats.cost->linesRemoved };
        auto json = std::string {};
        std::ignore = glz::write<WireOptions>(wire, json);
        return json + '\n';
    }

    /// @return Such as `152`, `42.0k` or `12.9M`.
    [[nodiscard]] std::string compactCount(uint64_t count)
    {
        if (count < 1'000)
            return std::format("{}", count);
        if (count < 1'000'000)
            return std::format("{:.1f}k", static_cast<double>(count) / 1'000);
        return std::format("{:.1f}M", static_cast<double>(count) / 1'000'000);
    }

    /// @return Such as `45s`, `14m 47s` or `2h 3m`.
    [[nodiscard]] std::string durationText(uint64_t ms)
    {
        auto const seconds = ms / 1'000;
        if (seconds < 60)
            return std::format("{}s", seconds);
        if (seconds < 3'600)
            return std::format("{}m {}s", seconds / 60, seconds % 60);
        return std::format("{}h {}m", seconds / 3'600, (seconds % 3'600) / 60);
    }

    [[nodiscard]] std::string tokensText(TokenCounts const& tokens)
    {
        auto const input = tokens.input + tokens.cacheWrite + tokens.cacheRead;
        auto const hit =
            input == 0
                ? std::string {}
                : std::format(" · {:.1f}% cache hit",
                              100.0 * static_cast<double>(tokens.cacheRead) / static_cast<double>(input));
        return std::format("{} out ({} thinking) · {} in · {} cache read · {} cache write{}",
                           compactCount(tokens.output),
                           compactCount(tokens.thinking),
                           compactCount(tokens.input),
                           compactCount(tokens.cacheRead),
                           compactCount(tokens.cacheWrite),
                           hit);
    }

    [[nodiscard]] std::string textStats(SessionStats const& stats, std::string_view session)
    {
        auto text = std::format("session   {}{}\n",
                                session,
                                stats.writtenBy.empty() ? std::string {}
                                                        : std::format(" (Claude Code {})", stats.writtenBy));
        if (!stats.firstTime.empty())
            text += std::format("time      {} – {} UTC · {} active\n",
                                stats.firstTime,
                                stats.lastTime,
                                durationText(stats.activeMs));
        text += std::format("tokens    {}\n", tokensText(stats.tokens));
        text += stats.cost ? std::format("cost      ${:.2f} as Claude Code reports it · API {} · tools {}\n",
                                         stats.cost->usd,
                                         durationText(stats.cost->apiMs),
                                         durationText(stats.cost->toolMs))
                           : std::string { "cost      not recorded yet: Claude Code records it on exit\n" };
        text += std::format("calls     {} API calls · {} API errors · {} prompts\n",
                            stats.apiCalls,
                            stats.apiErrors,
                            stats.prompts);
        auto tools = std::string {};
        for (auto const& tool: stats.tools)
            tools += std::format("{}{} {}", tools.empty() ? "" : " · ", tool.name, tool.calls);
        text += std::format("tools     {} calls{}{} · {} failed\n",
                            stats.toolCalls,
                            tools.empty() ? "" : ": ",
                            tools,
                            stats.toolFailures);
        text += std::format("subagents {} · {} API calls · {}\n",
                            stats.subagents.count,
                            stats.subagents.apiCalls,
                            tokensText(stats.subagents.tokens));
        text += std::format("compacted {} times\n", stats.compactions);
        for (auto const& pullRequest: stats.pullRequests)
            text += std::format("opened    #{} {}\n", pullRequest.number, pullRequest.url);
        return text;
    }
    // }}}

    /// Reads every subagent log beside @p log that was written in the run's directory, into @p reader.
    void readSubagents(SessionStatsReader& reader,
                       std::filesystem::path const& log,
                       std::string_view cwd,
                       core::platform::FileSystem const& fs,
                       ISessionLogOpener& opener)
    {
        auto const directory = log.parent_path() / log.stem() / SubagentDirectory;
        if (opener.isLinkedDirectory(directory))
            return;
        auto entries =
            fs.listDirectory(directory).value_or(std::vector<core::platform::FileSystem::DirectoryEntry> {});
        // The listing's order is the file system's: sorted, so that the same logs add up the same way.
        std::ranges::sort(entries, {}, &core::platform::FileSystem::DirectoryEntry::path);
        auto const root = core::pathFromUtf8(cwd).value_or(std::filesystem::path {});
        for (auto const& entry: entries)
        {
            auto const name = entry.path.filename().string();
            if (entry.isDirectory || entry.isSymlink || !name.starts_with(SubagentPrefix)
                || !name.ends_with(LogExtension))
                continue;
            auto stream = opener.open(entry.path);
            if (!stream)
                continue;
            // A subagent's lines are taken in only once its log is known to be the run's.
            auto lines = std::vector<std::string> {};
            auto const binding = detail::scanBound(
                **stream, root, [&](LogLine const& line) { lines.emplace_back(line.text); });
            if (!binding || *binding == detail::Binding::Outside)
                continue;
            reader.beginSubagent();
            for (auto const& line: lines)
                reader.readSubagent(LogLine { .text = line, .extent = LineExtent::Whole });
        }
    }
} // namespace

void SessionStatsReader::read(LogLine const& line)
{
    auto const fields = parsed(line);
    if (!fields)
    {
        ++_stats.notUnderstood;
        return;
    }
    if (!*fields)
        return;
    auto const& entry = **fields;
    if (entry.timestamp)
    {
        if (_stats.firstTime.empty())
            _stats.firstTime = sanitized(*entry.timestamp, MaxStatsFieldBytes);
        _stats.lastTime = sanitized(*entry.timestamp, MaxStatsFieldBytes);
    }
    if (entry.version)
        _stats.writtenBy = sanitized(*entry.version, MaxStatsFieldBytes);

    if (entry.type == "assistant")
    {
        if (entry.isApiErrorMessage.value_or(false))
        {
            ++_stats.apiErrors;
            return;
        }
        // A sidechain written into the session's own log, as Claude Code once did, is a subagent's.
        auto const isSidechain = entry.isSidechain.value_or(false);
        auto& tokens = isSidechain ? _stats.subagents.tokens : _stats.tokens;
        auto& calls = isSidechain ? _stats.subagents.apiCalls : _stats.apiCalls;
        if (entry.message && entry.message->id && entry.message->usage
            && firstSighting(_messages, *entry.message->id))
        {
            addUsage(tokens, *entry.message->usage);
            ++calls;
        }
        for (auto const& block: blocksOf(entry))
            if (block.type == "tool_use")
            {
                ++_stats.toolCalls;
                auto const name = sanitized(block.name.value_or(""), MaxStatsFieldBytes);
                auto const known = std::ranges::find(_tools, name, &ToolCount::name);
                if (known != _tools.end())
                    ++known->calls;
                else
                    _tools.push_back(ToolCount { .name = name, .calls = 1 });
            }
    }
    else if (entry.type == "user")
    {
        if (isPrompt(entry))
            ++_stats.prompts;
        _stats.toolFailures +=
            static_cast<uint64_t>(std::ranges::count_if(blocksOf(entry), [](StatsBlock const& block) {
                return block.type == "tool_result" && block.isError.value_or(false);
            }));
    }
    else if (entry.type == "system" && entry.subtype == "turn_duration")
        _stats.activeMs += entry.durationMs.value_or(0);
    else if (entry.type == "system" && entry.subtype == "compact_boundary")
        ++_stats.compactions;
    else if (entry.type == "pr-link" && entry.prNumber && entry.prUrl)
    {
        auto link =
            PullRequestLink { .number = *entry.prNumber, .url = sanitized(*entry.prUrl, MaxStatsUrlBytes) };
        if (_stats.pullRequests.size() < MaxStatsPullRequests
            && !std::ranges::contains(_stats.pullRequests, link))
            _stats.pullRequests.push_back(std::move(link));
    }
    else if (entry.type == "cost-state" && entry.totalCostUsd)
        // A resumed session records again on each exit: the last record covers it all.
        _stats.cost = SessionCost { .usd = roundedUsd(*entry.totalCostUsd),
                                    .apiMs = entry.totalApiMs.value_or(0),
                                    .apiMsWithoutRetries = entry.totalApiMsWithoutRetries.value_or(0),
                                    .toolMs = entry.totalToolMs.value_or(0),
                                    .totalMs = entry.totalMs.value_or(0),
                                    .linesAdded = entry.totalLinesAdded.value_or(0),
                                    .linesRemoved = entry.totalLinesRemoved.value_or(0) };
}

void SessionStatsReader::beginSubagent()
{
    ++_stats.subagents.count;
    _subagentMessages.clear();
}

void SessionStatsReader::readSubagent(LogLine const& line)
{
    auto const fields = parsed(line);
    if (!fields)
    {
        ++_stats.notUnderstood;
        return;
    }
    if (!*fields)
        return;
    auto const& entry = **fields;
    if (entry.type != "assistant" || entry.isApiErrorMessage.value_or(false) || !entry.message
        || !entry.message->id || !entry.message->usage)
        return;
    if (firstSighting(_subagentMessages, *entry.message->id))
    {
        addUsage(_stats.subagents.tokens, *entry.message->usage);
        ++_stats.subagents.apiCalls;
    }
}

SessionStats SessionStatsReader::finish() &&
{
    std::ranges::sort(_tools, [](ToolCount const& a, ToolCount const& b) {
        return a.calls != b.calls ? a.calls > b.calls : a.name < b.name;
    });
    if (_tools.size() > MaxStatsTools)
        _tools.resize(MaxStatsTools);
    _stats.tools = std::move(_tools);
    return std::move(_stats);
}

int runAgentStats(StatsRequest const& request,
                  core::Environment const& env,
                  core::platform::FileSystem const& fs,
                  ISessionLogOpener& opener,
                  std::ostream& out,
                  std::ostream& err)
{
    if (request.format != StatsFormat)
        return detail::refuse(
            err,
            TranscriptError::UnsupportedFormat,
            std::format("unsupported stats format {}; this contour prints {}", request.format, StatsFormat));
    auto readers = std::vector<SessionStatsReader> {};
    auto const scan = [&](std::istream& log, std::filesystem::path const& root) {
        auto& reader = readers.emplace_back();
        return detail::scanBound(log, root, [&](LogLine const& line) { reader.read(line); });
    };
    auto const query = detail::SessionLogQuery { .session = request.session, .cwd = request.cwd };
    auto const located = detail::locateSessionLog(query, env, fs, opener, scan);
    if (!located)
        return detail::refuse(err, located.error().error, detail::lookupMessage(located.error(), query));
    auto& reader = readers[located->candidate];
    readSubagents(reader, located->log, request.cwd, fs, opener);
    auto const stats = std::move(reader).finish();
    switch (request.output)
    {
        case StatsOutput::Text: out << textStats(stats, request.session); break;
        case StatsOutput::Json: out << jsonStats(stats, request.session); break;
    }
    return exitCode(ExitStatus::Ok);
}

} // namespace contour::cli
