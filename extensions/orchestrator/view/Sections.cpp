// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Sections.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/Unicode.hpp>

#include <libunicode/utf8_grapheme_segmenter.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/FailureWords.hpp>
#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Permissions.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/core/Sanitize.hpp>
#include <orchestrator/core/Timeline.hpp>
#include <orchestrator/core/Utf8.hpp>
#include <orchestrator/view/BoardMarkdown.hpp>
#include <orchestrator/view/StatusBoard.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    /// The most characters a row's text keeps once sanitised; it is cut to its cells when drawn.
    constexpr auto MaxRowChars = std::size_t { 400 };
    /// The most characters cut() sanitises before it measures.
    constexpr auto MaxCutChars = std::size_t { 4096 };

    /// The ellipsis a cut text ends in: one cell.
    constexpr auto Ellipsis = "…"sv;

    /// @return Whether @p text is printable ASCII alone: every byte a grapheme cluster of one cell, so that
    ///         its width is its size and it breaks anywhere, without the grapheme segmenter.
    [[nodiscard]] bool isPrintableAscii(std::string_view text) noexcept
    {
        return std::ranges::all_of(text, [](char c) { return c >= ' ' && c <= '~'; });
    }

    /// @param text Sanitised text, not empty.
    /// @param firstColumns The cells the first slice may take; positive.
    /// @param nextColumns The cells each further slice may take; positive.
    /// @return @p text in slices that fit, broken between grapheme clusters, a cluster wider than a slice
    ///         standing alone; at least one.
    [[nodiscard]] std::vector<std::string> slicesOf(std::string_view text, int firstColumns, int nextColumns)
    {
        auto slices = std::vector<std::string> {};
        auto slice = std::string {};
        auto used = 0;
        auto const limit = [&] {
            return slices.empty() ? firstColumns : nextColumns;
        };
        auto const endSlice = [&] {
            slices.push_back(std::move(slice));
            slice.clear();
            used = 0;
        };
        // Printable ASCII is a cluster of one cell per byte, broken in slices rather than cluster by cluster.
        if (isPrintableAscii(text))
            for (auto rest = text; !rest.empty();)
            {
                if (used >= limit())
                    endSlice();
                auto const taken = std::min(rest.size(), static_cast<std::size_t>(limit() - used));
                slice += rest.substr(0, taken);
                used += static_cast<int>(taken);
                rest.remove_prefix(taken);
            }
        else
            for (auto const& cluster: unicode::utf8_grapheme_segmenter(text))
            {
                auto const clusterWidth = core::tui::graphemeClusterWidth(cluster);
                if (used > 0 && used + clusterWidth > limit())
                    endSlice();
                for (auto const codepoint: cluster)
                    appendUtf8(slice, codepoint);
                used += clusterWidth;
            }
        endSlice();
        return slices;
    }

    /// Sanitised text for a row or a detail.
    [[nodiscard]] std::string clean(std::string_view text)
    {
        return displayText(text, MaxRowChars);
    }

    // {{{ what each state looks like

    // }}}
    // {{{ the words a row is made of

    /// @return Such as `#2334`, or a Jira key.

    /// @return Such as `pr-2334`, or a topic's text; the run's id when the item has no slug.
    [[nodiscard]] std::string itemName(Run const& run)
    {
        if (std::holds_alternative<TopicRef>(run.item))
            return run.topic;
        auto const slug = itemSlug(run.item);
        return slug ? *slug : run.id;
    }

    /// @return What the configuration calls the run's task; its id when no task has it.
    [[nodiscard]] std::string_view taskLabel(Config const* config, Run const& run) noexcept
    {
        auto const* const task = config != nullptr ? config->findTask(run.task) : nullptr;
        return task != nullptr ? std::string_view { task->label } : std::string_view { run.task };
    }

    constexpr auto SecondMs = uint64_t { 1000 };
    constexpr auto MinuteMs = 60 * SecondMs;
    constexpr auto HourMs = 60 * MinuteMs;

    /// The unit an age is shown in: ages below @ref below count @ref unitMs, suffixed @ref suffix.
    struct AgeUnit
    {
        uint64_t below = 0;           ///< The first age shown in a larger unit.
        uint64_t unitMs = 0;          ///< What one step of the shown number is.
        std::string_view suffix = {}; ///< Such as `s`.
    };

    /// The units ages are shown in, smallest first; the last takes every age.
    constexpr auto AgeUnits = std::array {
        AgeUnit { .below = MinuteMs, .unitMs = SecondMs, .suffix = "s" },
        AgeUnit { .below = HourMs, .unitMs = MinuteMs, .suffix = "m" },
        AgeUnit { .below = std::numeric_limits<uint64_t>::max(), .unitMs = HourMs, .suffix = "h" },
    };

    [[nodiscard]] constexpr AgeUnit const& ageUnitOf(uint64_t ms) noexcept
    {
        return *std::ranges::find_if(AgeUnits, [ms](AgeUnit const& unit) { return ms < unit.below; });
    }

    /// @return Such as `45s`, `6m` or `3h`.
    [[nodiscard]] std::string ageText(uint64_t ms)
    {
        auto const& unit = ageUnitOf(ms);
        return std::format("{}{}", ms / unit.unitMs, unit.suffix);
    }

    constexpr auto TrustPromptHintAfterMs =
        static_cast<uint64_t>(std::chrono::milliseconds { TrustPromptHintAfter }.count());

    /// @return When the run's row starts to hint at a trust prompt: it runs, its agent never reported a
    /// status
    ///         (Run::lastStatus, which the run keeps across a restart of the guest), and when its tab opened
    ///         is known; std::nullopt otherwise.
    [[nodiscard]] std::optional<uint64_t> trustHintFrom(Run const& run) noexcept
    {
        if (!std::holds_alternative<Running>(run.state) || run.lastStatus || run.runningSinceMs == 0)
            return std::nullopt;
        return run.runningSinceMs + TrustPromptHintAfterMs;
    }

    /// What an agent's row says it is doing.
    enum class AgentLook : uint8_t
    {
        Reported = 0,  ///< What its agent last reported, or its state, with its age.
        AwaitsTrust,   ///< Nothing reported for TrustPromptHintAfter: a folder-trust prompt may wait in its
                       ///< tab.
        OutsideConfig, ///< Outside the configuration and untouched: left alone until the user ends it.
    };

    /// @return Whether nothing has happened to the run since its agent started: it runs, is blocked or done.
    ///         Once the user ends it, what it is doing says more than where it belongs.
    [[nodiscard]] bool isUntouched(Run const& run) noexcept
    {
        return std::holds_alternative<Running>(run.state) || std::holds_alternative<Blocked>(run.state)
               || std::holds_alternative<Done>(run.state);
    }

    [[nodiscard]] AgentLook agentLookOf(Run const& run, RunScope scope, uint64_t nowMonoMs) noexcept
    {
        if (scope == RunScope::Outside && isUntouched(run))
            return AgentLook::OutsideConfig;
        if (scope == RunScope::Outside)
            return AgentLook::Reported;
        auto const hintFrom = trustHintFrom(run);
        return hintFrom && nowMonoMs >= *hintFrom ? AgentLook::AwaitsTrust : AgentLook::Reported;
    }

    /// @return Whether the run's row shows its age at @p nowMonoMs: it shows what was reported, it runs or is
    ///         blocked, and its age is known.
    [[nodiscard]] bool showsAge(Run const& run, RunScope scope, uint64_t nowMonoMs) noexcept
    {
        auto const ages =
            std::holds_alternative<Running>(run.state) || std::holds_alternative<Blocked>(run.state);
        return ages && agentLookOf(run, scope, nowMonoMs) == AgentLook::Reported && run.runningSinceMs != 0
               && nowMonoMs >= run.runningSinceMs;
    }

    /// @return The run's age since its tab opened, after a space; empty when that is not known.
    [[nodiscard]] std::string ageOf(Run const& run, RunScope scope, uint64_t nowMonoMs)
    {
        if (!showsAge(run, scope, nowMonoMs))
            return {};
        return std::format(" {}", ageText(nowMonoMs - run.runningSinceMs));
    }

    /// What a run's row says when it is outside the configuration.
    constexpr auto OutsideStatus = "outside the configuration"sv;

    /// What the details of a run outside the configuration always say, first.
    [[nodiscard]] DetailLine outsideDetail()
    {
        return DetailLine { .label = "", .value = std::string { OutsideStatus }, .role = Role::Warning };
    }

    /// Whether rows name their repository.
    enum class RepoPrefix : uint8_t
    {
        Omitted, ///< One repository is configured.
        Shown,   ///< More than one is.
    };

    /// What every row needs to know about the lists it is part of.
    struct Lists
    {
        Engine const* engine = nullptr;          ///< The engine, which tells a run's scope.
        Config const* config = nullptr;          ///< The configuration.
        RepoPrefix prefix = RepoPrefix::Omitted; ///< Whether rows name their repository.
        uint64_t nowMonoMs = 0;                  ///< Now.
    };

    /// @return @p repo, when the rows name their repository.
    [[nodiscard]] std::string prefixOf(Lists const& lists, std::string_view repo)
    {
        return lists.prefix == RepoPrefix::Shown ? clean(repo) : std::string {};
    }

    /// What a running run's row says of its agent: the reason line of an agent waiting on its own work (idle,
    /// such as `Waiting for Build pace`), else the name of what it reported.
    [[nodiscard]] std::string runningText(Lists const& lists, Run const& run)
    {
        auto const status = run.lastStatus.value_or(AgentStatus::Working);
        if (auto const message = lists.engine->reportedMessageOf(run);
            status == AgentStatus::Idle && !message.empty())
            return clean(message);
        return std::string { agentStatusName(status) };
    }

    /// What a done run's row says: its agent's turn ended with nothing in flight, so it waits for you,
    /// whether it finished its task or asked you something.
    constexpr auto WaitingForYou = "waiting for you"sv;

    /// What a pull request's or an issue's row says while an agent works on it.
    struct AgentCue
    {
        std::string status; ///< The row's status: the agent's mark and state, such as `○ agent working`.
        Role role;          ///< How it is drawn: as the agent's own row draws its state.
        DetailLine detail;  ///< The details' first line: the task and the state.
    };

    /// @return What the row of @p item says while an active run works on it; std::nullopt while none does.
    [[nodiscard]] std::optional<AgentCue> agentCueFor(Lists const& lists,
                                                      std::string_view repo,
                                                      ItemRef const& item)
    {
        auto const* const run = lists.engine->activeRunOn(repo, item);
        if (run == nullptr)
            return std::nullopt;
        auto const reported = [&](AgentStatus fallback) {
            return std::string { agentStatusName(run->lastStatus.value_or(fallback)) };
        };
        auto const [mark, role, state] = std::visit(
            Overloaded {
                [&](Running const&) { return std::tuple { "○"sv, Role::Accent, runningText(lists, *run) }; },
                [&](Blocked const&) {
                    return std::tuple { "●"sv, Role::Warning, reported(AgentStatus::Blocked) };
                },
                [](Done const&) {
                    return std::tuple { "●"sv, Role::Warning, std::string { WaitingForYou } };
                },
                [&](auto const&) { return std::tuple { "◌"sv, Role::Dim, runStateText(*run) }; },
            },
            run->state);
        return AgentCue {
            .status = std::format("{} agent {}", mark, state),
            .role = role,
            .detail =
                DetailLine { .label = "agent",
                             .value = clean(std::format(
                                 "{} · {} · enter shows its tab", taskLabel(lists.config, *run), state)),
                             .role = role },
        };
    }

    // }}}
    // {{{ one row per item

    [[nodiscard]] Row pullRequestRow(Lists const& lists, std::string_view repo, ChangeRequest const& request)
    {
        auto const& ci = entryOf(CiLooks, request.ci);
        auto const& review = entryOf(ReviewLooks, request.review);
        auto const cue = agentCueFor(lists, repo, ChangeRequestRef { request.number });
        // An agent at work comes first, so that a short details panel still shows it.
        auto details = std::vector<DetailLine> {};
        if (cue)
            details.push_back(cue->detail);
        details.push_back(DetailLine { .label = "branch",
                                       .value = clean(std::format("{} → {}", request.head, request.base)) });
        details.push_back(
            DetailLine { .label = "checks", .value = std::string { ci.word }, .role = ci.role });
        for (auto const& check: request.failingChecks)
            details.push_back(DetailLine {
                .label = "", .value = clean(std::format("✗ {}", check)), .role = Role::Failing });
        if (!review.word.empty())
            details.push_back(
                DetailLine { .label = "review", .value = std::string { review.word }, .role = review.role });
        if (request.readiness == Readiness::Draft)
            details.push_back(DetailLine { .label = "state", .value = "draft" });
        if (request.origin == HeadOrigin::Fork)
            details.push_back(DetailLine { .label = "head", .value = "in a fork" });
        details.push_back(DetailLine { .label = "url", .value = clean(request.url), .role = Role::Dim });
        return Row { .kind = RowKind::PullRequest,
                     .mark = std::string { ci.mark },
                     .markRole = ci.role,
                     .prefix = prefixOf(lists, repo),
                     .text = clean(std::format("#{} {}{}",
                                               request.number,
                                               request.title,
                                               request.readiness == Readiness::Draft ? " · draft"sv : ""sv)),
                     .status = !cue                  ? std::string { review.word }
                               : review.word.empty() ? cue->status
                                                     : std::format("{} · {}", cue->status, review.word),
                     .statusRole = cue ? cue->role : review.role,
                     .repo = std::string { repo },
                     .item = ChangeRequestRef { request.number },
                     .title = clean(request.title),
                     .details = std::move(details) };
    }

    [[nodiscard]] Row issueRow(Lists const& lists, std::string_view repo, Issue const& issue)
    {
        auto const cue = agentCueFor(lists, repo, issue.item);
        auto details = std::vector<DetailLine> {};
        if (cue)
            details.push_back(cue->detail);
        details.push_back(DetailLine { .label = "issue", .value = clean(itemLabel(issue.item)) });
        details.push_back(DetailLine { .label = "url", .value = clean(issue.url), .role = Role::Dim });
        return Row { .kind = RowKind::Issue,
                     .prefix = prefixOf(lists, repo),
                     .text = clean(std::format("{} {}", itemLabel(issue.item), issue.title)),
                     .status = cue ? cue->status : std::string {},
                     .statusRole = cue ? cue->role : Role::Dim,
                     .repo = std::string { repo },
                     .item = issue.item,
                     .title = clean(issue.title),
                     .details = std::move(details) };
    }

    // {{{ statistics

    /// The most tools a run's details name.
    constexpr auto MaxShownTools = std::size_t { 5 };

    /// @return Such as `152`, `42.3k` or `12.9M`.
    [[nodiscard]] std::string countText(uint64_t count)
    {
        if (count < 1'000)
            return std::format("{}", count);
        if (count < 1'000'000)
            return std::format("{:.1f}k", static_cast<double>(count) / 1'000);
        return std::format("{:.1f}M", static_cast<double>(count) / 1'000'000);
    }

    /// @return Such as `45s`, `14m 47s` or `2h 3m`.
    [[nodiscard]] std::string spanText(uint64_t ms)
    {
        auto const seconds = ms / 1'000;
        if (seconds < 60)
            return std::format("{}s", seconds);
        if (seconds < 3'600)
            return std::format("{}m {}s", seconds / 60, seconds % 60);
        return std::format("{}h {}m", seconds / 3'600, (seconds % 3'600) / 60);
    }

    /// @return Such as `1 prompt` or `2 prompts`.
    [[nodiscard]] std::string counted(uint64_t count, std::string_view noun)
    {
        return std::format("{} {}{}", count, noun, count == 1 ? "" : "s");
    }

    /// @return @p parts joined with ` · `, the empty ones left out.
    [[nodiscard]] std::string joined(std::initializer_list<std::string> parts)
    {
        auto text = std::string {};
        for (auto const& part: parts)
            if (!part.empty())
                text += text.empty() ? part : std::format(" · {}", part);
        return text;
    }

    /// The details a run's statistics add: what its agent spent and did, and how long it waited for the user.
    [[nodiscard]] std::vector<DetailLine> statsDetails(Run const& run)
    {
        if (run.failure)
            return {};
        auto const times = timesOf(run.history);
        // Until the first read a minute in, there is nothing to say: the panel keeps its room for the rest.
        if (!run.stats)
            return {};
        auto const& stats = *run.stats;
        auto details = std::vector<DetailLine> {};
        details.push_back(DetailLine {
            .label = "time",
            .value =
                joined({ times.wallMs ? std::format("{} wall", spanText(*times.wallMs)) : std::string {},
                         std::format("{} active", spanText(stats.activeMs)),
                         times.waitingMs != 0 ? std::format("{} waiting for you", spanText(times.waitingMs))
                                              : std::string {} }) });
        details.push_back(DetailLine {
            .label = "tokens",
            .value = joined({ std::format("{} out", countText(stats.tokens.output)),
                              std::format("{} in", countText(stats.tokens.input)),
                              std::format("{} cache read", countText(stats.tokens.cacheRead)),
                              std::format("{} cache write", countText(stats.tokens.cacheWrite)) }) });
        if (auto const hit = cacheHitOf(stats.tokens))
            details.push_back(
                DetailLine { .label = "cache", .value = std::format("{:.1f}% hit", *hit * 100) });
        details.push_back(
            stats.cost
                ? DetailLine { .label = "cost",
                               .value = std::format("${:.2f}, as Claude Code reports it", stats.cost->usd) }
                : DetailLine {
                      .label = "cost", .value = "recorded when the agent exits", .role = Role::Dim });
        details.push_back(DetailLine {
            .label = "calls",
            .value =
                joined({ counted(stats.apiCalls, "API call"),
                         counted(stats.prompts, "prompt"),
                         stats.apiErrors != 0 ? counted(stats.apiErrors, "API error") : std::string {} }),
            .role = stats.apiErrors != 0 ? Role::Warning : Role::Text });
        auto tools = std::string {};
        for (auto const& tool: stats.tools | std::views::take(MaxShownTools))
            tools += std::format("{}{} {}", tools.empty() ? "" : " · ", tool.name, tool.calls);
        details.push_back(DetailLine {
            .label = "tools",
            .value = clean(joined({ tools.empty() ? counted(stats.toolCalls, "call") : tools,
                                    stats.toolFailures != 0 ? std::format("{} failed", stats.toolFailures)
                                                            : std::string {} })) });
        if (stats.subagents.count != 0 || stats.compactions != 0)
            details.push_back(DetailLine { .label = "agents",
                                           .value = std::format("{} ({} out) · {}",
                                                                counted(stats.subagents.count, "subagent"),
                                                                countText(stats.subagents.tokens.output),
                                                                counted(stats.compactions, "compaction")) });
        for (auto const& pullRequest: stats.pullRequests)
            details.push_back(
                DetailLine { .label = "opened",
                             .value = clean(std::format("#{} {}", pullRequest.number, pullRequest.url)),
                             .fit = DetailFit::Cut });
        return details;
    }

    // }}}
    // {{{ the timeline

    /// The bars a chart is drawn with, lowest first; an hour with nothing draws the first.
    constexpr auto Bars = std::array { "·"sv, "▁"sv, "▂"sv, "▃"sv, "▄"sv, "▅"sv, "▆"sv, "▇"sv, "█"sv };

    /// @return @p values as bars, each scaled to the largest; a value above nothing draws at least the
    /// lowest.
    [[nodiscard]] std::string barsOf(std::span<uint64_t const> values)
    {
        auto const top = std::ranges::max(values);
        auto bars = std::string {};
        for (auto const value: values)
        {
            auto const level = value == 0 || top == 0
                                   ? std::size_t { 0 }
                                   : std::max<std::size_t>(1, (value * (Bars.size() - 1) + top - 1) / top);
            bars += Bars[level];
        }
        return bars;
    }

    template <typename T>
    [[nodiscard]] std::array<uint64_t, TimelineBuckets> widened(std::array<T, TimelineBuckets> const& values)
    {
        auto wide = std::array<uint64_t, TimelineBuckets> {};
        std::ranges::copy(values, wide.begin());
        return wide;
    }

    /// One line of the timeline: what it charts, and what that adds up to.
    struct ChartLine
    {
        std::string_view label;
        std::array<uint64_t, TimelineBuckets> (*series)(Timeline const&);
        std::string (*summary)(Timeline const&);
    };

    /// The timeline's lines, in order: a new one is a row here.
    constexpr auto ChartLines = std::array {
        ChartLine { .label = "agents active"sv,
                    .series = [](Timeline const& timeline) { return widened(timeline.active); },
                    .summary =
                        [](Timeline const& timeline) {
                            return std::format(
                                "peak {} · now {}", std::ranges::max(timeline.active), timeline.activeNow);
                        } },
        ChartLine { .label = "tokens out"sv,
                    .series = [](Timeline const& timeline) { return timeline.tokens; },
                    .summary =
                        [](Timeline const& timeline) {
                            auto total = uint64_t { 0 };
                            for (auto const tokens: timeline.tokens)
                                total += tokens;
                            return std::format("{} in 24h", countText(total));
                        } },
        ChartLine { .label = "runs started"sv,
                    .series = [](Timeline const& timeline) { return widened(timeline.started); },
                    .summary =
                        [](Timeline const& timeline) {
                            auto started = uint64_t { 0 };
                            auto ended = uint64_t { 0 };
                            for (auto const index: std::views::iota(std::size_t { 0 }, TimelineBuckets))
                            {
                                started += timeline.started[index];
                                ended += timeline.ended[index];
                            }
                            return std::format("{} started · {} ended", started, ended);
                        } },
    };

    void appendSection(std::vector<Row>& rows,
                       std::string heading,
                       std::string_view placeholder,
                       std::vector<Row> items);

    /// Appends one repository's @p items as a group: its header, which folds them, then the items, each
    /// marked with the group and whether it is folded. A repository with no items has no group.
    void appendGroup(std::vector<Row>& rows,
                     Engine const& engine,
                     std::string group,
                     std::string_view repo,
                     std::vector<Row> items)
    {
        if (items.empty())
            return;
        auto const fold = engine.isFolded(group) ? Fold::Folded : Fold::Open;
        rows.push_back(
            Row { .kind = RowKind::Group,
                  .mark = fold == Fold::Folded ? "▸" : "▾",
                  .markRole = Role::Dim,
                  .text = clean(std::format("{} · {}", repo, items.size())),
                  .repo = std::string { repo },
                  .group = group,
                  .fold = fold,
                  .title = clean(repo),
                  .details = { DetailLine {
                      .label = "", .value = "folded: enter or z shows its items", .role = Role::Dim } } });
        for (auto& item: items)
        {
            item.prefix.clear();
            item.group = group;
            item.fold = fold;
            rows.push_back(std::move(item));
        }
    }

    /// Appends the timeline of the last day, when a run was active or dispatched in it.
    void appendTimeline(std::vector<Row>& rows, Engine const& engine)
    {
        auto const timeline = timelineOf(engine.runs(), engine.wallMs());
        if (timeline == Timeline {})
            return;
        auto chart = std::vector<Row> {};
        for (auto const& line: ChartLines)
            chart.push_back(Row { .kind = RowKind::Chart,
                                  .prefix = std::string { line.label },
                                  .text = barsOf(line.series(timeline)),
                                  .status = line.summary(timeline),
                                  .statusRole = Role::Dim });
        appendSection(rows, "TIMELINE · LAST 24 HOURS, AN HOUR A BAR", {}, std::move(chart));
    }

    // }}}

    /// The details every run row shows.
    [[nodiscard]] std::vector<DetailLine> runDetails(Lists const& lists, Run const& run)
    {
        auto details =
            std::vector<DetailLine> { DetailLine { .label = "task",
                                                   .value = clean(taskLabel(lists.config, run)) },
                                      DetailLine { .label = "model", .value = clean(run.model) },
                                      DetailLine { .label = "state", .value = clean(runStateText(run)) } };
        // What the agent spent and did comes before where it worked: it is what a finished run is opened for.
        std::ranges::move(statsDetails(run), std::back_inserter(details));
        details.push_back(DetailLine { .label = "branch", .value = clean(run.branch) });
        details.push_back(DetailLine { .label = "worktree", .value = clean(run.worktree) });
        // A topic has no web page: its text is what it is about.
        if (std::holds_alternative<TopicRef>(run.item))
            details.push_back(DetailLine { .label = "topic", .value = clean(run.topic) });
        else
            details.push_back(DetailLine { .label = "url", .value = clean(run.url), .role = Role::Dim });
        // What its agent may do without asking, as it was started: read from the run, not the configuration.
        // Last, below where it works, which a short panel must keep.
        if (auto const perms = permissionsText(run.permissions, run.settings); !perms.empty())
            details.push_back(DetailLine { .label = "perms", .value = clean(perms) });
        return details;
    }

    /// The most lines of a board an agent's details show: the `b` screen shows it whole.
    constexpr auto MaxBoardDetailLines = std::size_t { 60 };

    /// The run's board, rendered, as the last lines of its details: what its agent says it is doing, read for
    /// the tab while the run is selected (Dashboard). Nothing while it was not read or holds nothing.
    [[nodiscard]] std::vector<DetailLine> boardDetails(Lists const& lists, Run const& run)
    {
        auto const* const board = lists.engine->board(run.id);
        if (board == nullptr || board->state != BoardState::Read)
            return {};
        auto const lines = renderBoardMarkdown(displayLines(board->bytes, MaxBoardLines, MaxBoardLineChars));
        if (lines.empty())
            return {};
        auto details = std::vector<DetailLine> { DetailLine {} };
        details.push_back(
            DetailLine { .label = "board", .value = "STATUS.md · b shows it whole", .role = Role::Dim });
        for (auto const& line: lines | std::views::take(MaxBoardDetailLines))
            details.push_back(
                DetailLine { .label = "",
                             .value = line.text,
                             .role = line.role,
                             .fit = line.fit == BoardFit::Cut ? DetailFit::Aligned : DetailFit::Wrap });
        if (lines.size() > MaxBoardDetailLines)
            details.push_back(DetailLine { .label = "", .value = "…", .role = Role::Dim });
        return details;
    }

    [[nodiscard]] Row agentRow(Lists const& lists, Run const& run)
    {
        auto const scope = lists.engine->scopeOf(run);
        auto const look = agentLookOf(run, scope, lists.nowMonoMs);
        auto const age = ageOf(run, scope, lists.nowMonoMs);
        auto const reported = [&](AgentStatus fallback) {
            return agentStatusName(run.lastStatus.value_or(fallback));
        };
        auto [mark, role, status] = std::visit(
            Overloaded {
                [&](Running const&) {
                    return std::tuple { "○"sv,
                                        Role::Text,
                                        std::format("{}{}", runningText(lists, run), age) };
                },
                [&](Blocked const&) {
                    return std::tuple { "●"sv,
                                        Role::Warning,
                                        std::format("{}{}", reported(AgentStatus::Blocked), age) };
                },
                [&](Done const&) {
                    auto const ends =
                        lists.config != nullptr && lists.config->endDoneAfterFor(run.task).count() != 0
                            ? "ends by itself"sv
                            : "e ends it"sv;
                    return std::tuple { "●"sv, Role::Warning, std::format("{} · {}", WaitingForYou, ends) };
                },
                [&](auto const&) { return std::tuple { "◌"sv, Role::Dim, runStateText(run) }; },
            },
            run.state);
        // An active run outside the configuration stays endable: its cleanup works from what the run stored.
        auto offers = std::vector<KeyCommand> { KeyCommand::Board, KeyCommand::Transcript };
        if (countsAsActive(run))
            offers.push_back(KeyCommand::EndRun);
        // What the row stands out for comes first, so that a short details panel still shows it.
        auto details = std::vector<DetailLine> {};
        if (scope == RunScope::Outside)
            details.push_back(outsideDetail());
        auto statusRole = role == Role::Text ? Role::Dim : role;
        switch (look)
        {
            case AgentLook::Reported: break;
            case AgentLook::AwaitsTrust:
                // Only a pointer to the tab: the prompt is the user's to answer.
                status = "waiting? see its tab";
                statusRole = Role::Warning;
                details.push_back(DetailLine { .label = "",
                                               .value = "may be waiting for a folder-trust prompt in its tab",
                                               .role = Role::Warning });
                break;
            case AgentLook::OutsideConfig:
                status = OutsideStatus;
                statusRole = Role::Warning;
                break;
        }
        if (std::holds_alternative<Done>(run.state) && lists.config != nullptr
            && lists.config->endDoneAfterFor(run.task).count() != 0)
            details.push_back(DetailLine {
                .label = "ends",
                .value = std::format(
                    "by itself once it waited for you for {} (end_done_after); e ends it now",
                    ageText(static_cast<uint64_t>(lists.config->endDoneAfterFor(run.task).count()))),
                .role = Role::Warning });
        std::ranges::move(runDetails(lists, run), std::back_inserter(details));
        std::ranges::move(boardDetails(lists, run), std::back_inserter(details));
        // A run whose STATUS.md was found says so: `b` shows it.
        if (lists.engine->hasBoard(run.id))
            status = std::format("{} {}", BoardCue, status);
        auto const text = std::format("{} · {} · {}", itemName(run), taskLabel(lists.config, run), run.model);
        return Row { .kind = RowKind::Agent,
                     .mark = std::string { mark },
                     .markRole = role,
                     .prefix = prefixOf(lists, run.repo),
                     .text = clean(text),
                     .status = clean(status),
                     .statusRole = statusRole,
                     .run = run.id,
                     .offers = std::move(offers),
                     .title = clean(text),
                     .details = std::move(details) };
    }

    [[nodiscard]] Row finishedRow(Lists const& lists, Run const& run, Finished const& finished)
    {
        auto const isChecking = !std::holds_alternative<Finished>(run.state);
        auto const kept = finished.fate == Fate::Kept;
        // Outside the configuration a kept worktree's removal is never tried again: its checks need a
        // configured repository.
        auto const scope = lists.engine->scopeOf(run);
        auto const isRetryable = kept && !isChecking && scope != RunScope::Outside;
        // Its outcome is the row's status; where it belongs is in its details.
        auto status = std::string {};
        auto statusRole = Role::Passing;
        if (isChecking)
            std::tie(status, statusRole) = std::tuple { std::string { "checking again" }, Role::Dim };
        else if (run.failure)
            std::tie(status, statusRole) = std::tuple { std::string { "failed" }, Role::Failing };
        else if (kept)
            std::tie(status, statusRole) =
                std::tuple { std::format("kept: {} {}",
                                         finished.entries.size(),
                                         finished.entries.size() == 1 ? "entry" : "entries"),
                             Role::Warning };
        else
            status = "removed";

        // A new run on its item goes through the dispatch form, as every dispatch does. Its board went with
        // its worktree, its transcript did not.
        auto offers =
            keepsWorktree(run)
                ? std::vector<KeyCommand> { KeyCommand::Board, KeyCommand::Transcript, KeyCommand::Dispatch }
                : std::vector<KeyCommand> { KeyCommand::Transcript, KeyCommand::Dispatch };
        if (isRetryable)
            offers.push_back(KeyCommand::RetryRemoval);
        if (!isChecking)
            offers.push_back(KeyCommand::Dismiss);

        auto details = std::vector<DetailLine> {};
        if (scope == RunScope::Outside)
            details.push_back(outsideDetail());
        std::ranges::move(runDetails(lists, run), std::back_inserter(details));
        if (run.failure)
        {
            details.push_back(DetailLine {
                .label = "failure", .value = clean(plainFailure(run.failure->why)), .role = Role::Failing });
            if (auto const* const words = failureWordsOf(run.failure->why); words && !words->detail.empty())
                details.push_back(
                    DetailLine { .label = "", .value = clean(words->detail), .role = Role::Dim });
        }
        for (auto const& entry: finished.entries)
            details.push_back(DetailLine { .label = "", .value = clean(entry), .role = Role::Warning });
        if (isRetryable)
            details.push_back(
                DetailLine { .label = "", .value = "x tries the removal again", .role = Role::Dim });

        auto const text = std::format(
            "{} · {} · {}", itemName(run), taskLabel(lists.config, run), endedText(finished.ended));
        auto const isNew = finished.seen == Seen::New;
        return Row { .kind = RowKind::Finished,
                     .mark = isNew ? std::string { "●" } : std::string {},
                     .markRole = Role::Text,
                     .prefix = prefixOf(lists, run.repo),
                     .text = clean(text),
                     .status = std::move(status),
                     .statusRole = statusRole,
                     .run = run.id,
                     .offers = std::move(offers),
                     .onOpen = isNew ? OpenEffect::MarkSeen : OpenEffect::None,
                     .title = clean(text),
                     .details = std::move(details) };
    }

    /// @return The line that removes @p orphan by hand, run from anywhere: `git -C <checkout> worktree remove
    ///         <path>`, each argument one shell word; without `-C` while the checkout is not known; and
    ///         std::nullopt when either path does not show as it is, since what is drawn could then name
    ///         another directory -- a live run's worktree, say.
    [[nodiscard]] std::optional<std::string> removalCommand(std::optional<std::string_view> checkout,
                                                            WorktreeEntry const& orphan)
    {
        if (!showsAsItIs(orphan.path) || (checkout && !showsAsItIs(*checkout)))
            return std::nullopt;
        auto argv = std::vector<std::string> { "git" };
        if (checkout)
            argv.insert(argv.end(), { "-C", std::string { *checkout } });
        argv.insert(argv.end(), { "worktree", "remove", orphan.path });
        // Not clean(): both paths were checked, and its cut would lose the end of a long one.
        return shellQuoted(argv);
    }

    /// A worktree no run accounts for. Nothing here removes it: the details say how to, by hand, with the
    /// same safety git gives anyone (it refuses a worktree with changes).
    [[nodiscard]] Row orphanRow(Lists const& lists, std::string_view repo, WorktreeEntry const& orphan)
    {
        auto const isDetached = !orphan.branch;
        auto const branch = isDetached ? std::string { "detached" } : clean(*orphan.branch);
        // The path is the title; what it is and what to do about it come first, so that a short panel still
        // shows them.
        auto details = std::vector<DetailLine> {
            DetailLine { .label = "",
                         .value =
                             "no agent run uses this worktree any more; the orchestrator never removes it",
                         .role = Role::Warning },
            DetailLine { .label = "",
                         .value =
                             "a run ended without removing it, or it was made by hand under worktree_root",
                         .role = Role::Dim },
            DetailLine { .label = "branch", .value = branch, .fit = DetailFit::Cut },
        };
        if (auto command = removalCommand(lists.engine->mainCheckoutOf(repo), orphan))
            details.push_back(
                DetailLine { .label = "to remove", .value = std::move(*command), .fit = DetailFit::Cut });
        else
            details.push_back(DetailLine { .label = "to remove",
                                           .value = "its path holds characters that cannot be shown; "
                                                    "git worktree list names it",
                                           .role = Role::Warning });
        details.push_back(
            isDetached ? DetailLine { .label = "",
                                      .value = "its HEAD is detached: commits no branch holds are lost "
                                               "with it",
                                      .role = Role::Warning }
                       : DetailLine { .label = "",
                                      .value = "git refuses while it holds changes; its branch stays",
                                      .role = Role::Dim });
        return Row { .kind = RowKind::Orphan,
                     .prefix = prefixOf(lists, repo),
                     .text = clean(orphan.path),
                     .status = branch,
                     .repo = std::string { repo },
                     .title = clean(orphan.path),
                     .details = std::move(details) };
    }

    // }}}

    /// Appends a section: a spacer before all but the first, its heading, and its items or its placeholder.
    void appendSection(std::vector<Row>& rows,
                       std::string heading,
                       std::string_view placeholder,
                       std::vector<Row> items)
    {
        if (!rows.empty())
            rows.push_back(Row { .kind = RowKind::Spacer });
        rows.push_back(Row { .kind = RowKind::Heading, .text = std::move(heading) });
        if (items.empty())
            rows.push_back(Row { .kind = RowKind::Placeholder, .text = std::string { placeholder } });
        std::ranges::move(items, std::back_inserter(rows));
    }

    /// Appends the AGENTS and FINISHED sections: every stored run, in the engine's order.
    void appendRunSections(std::vector<Row>& rows, Engine const& engine, Lists const& lists)
    {
        auto agents = std::vector<Row> {};
        auto finished = std::vector<Row> {};
        for (auto const& run: engine.runs())
        {
            if (auto const* const entry = finishedEntryOf(run))
                finished.push_back(finishedRow(lists, run, *entry));
            else
                agents.push_back(agentRow(lists, run));
        }
        auto const active = std::ranges::count_if(engine.runs(), countsAsActive);
        appendSection(rows,
                      lists.config != nullptr
                          ? std::format("AGENTS · {} OF {} RUNNING", active, lists.config->maxAgents())
                          : std::format("AGENTS · {} RUNNING", active),
                      "no agents",
                      std::move(agents));
        appendSection(rows,
                      std::format("FINISHED · {} NEW", newFinishedCount(engine.runs())),
                      "nothing finished",
                      std::move(finished));
    }

    /// The rows without a configuration: what to configure, then the runs the store holds, which stay
    /// endable while a typo keeps the configuration from loading.
    [[nodiscard]] std::vector<Row> unconfiguredRows(Engine const& engine, uint64_t nowMonoMs)
    {
        auto const hasRuns = !engine.runs().empty();
        auto rows = std::vector<Row> {
            Row { .kind = RowKind::Notice,
                  .text = hasRuns ? "Only the runs are listed until the orchestrator is configured."
                                  : "Nothing to list until the orchestrator is configured." },
            Row { .kind = RowKind::Notice, .text = "Configure daemon.extensions.orchestrator.config:" },
            Row { .kind = RowKind::Notice, .text = "worktree_root, models, repos and tasks." },
            Row { .kind = RowKind::Notice, .text = "The line above says what is missing or wrong." },
        };
        if (hasRuns)
            appendRunSections(rows, engine, Lists { .engine = &engine, .nowMonoMs = nowMonoMs });
        return rows;
    }
} // namespace

std::optional<uint64_t> nextRowChange(Engine const& engine, uint64_t nowMonoMs)
{
    auto earliest = std::optional<uint64_t> {};
    auto const consider = [&](uint64_t at) {
        earliest = earliest ? std::min(*earliest, at) : at;
    };
    for (auto const& run: engine.runs())
    {
        auto const scope = engine.scopeOf(run);
        if (auto const hintFrom = trustHintFrom(run);
            scope != RunScope::Outside && hintFrom && nowMonoMs < *hintFrom)
            consider(*hintFrom);
        if (!showsAge(run, scope, nowMonoMs))
            continue;
        auto const age = nowMonoMs - run.runningSinceMs;
        auto const unitMs = ageUnitOf(age).unitMs;
        consider(run.runningSinceMs + (((age / unitMs) + 1) * unitMs));
    }
    return earliest;
}

std::vector<Row> buildRows(Engine const& engine, uint64_t nowMonoMs)
{
    auto const* const config = engine.config();
    if (config == nullptr)
        return unconfiguredRows(engine, nowMonoMs);

    auto const repos = engine.repositories();
    auto const lists = Lists { .engine = &engine,
                               .config = config,
                               .prefix = repos.size() > 1 ? RepoPrefix::Shown : RepoPrefix::Omitted,
                               .nowMonoMs = nowMonoMs };

    // One group per repository: its header names the repository, so its items do not.
    auto pullRequests = std::vector<Row> {};
    auto issues = std::vector<Row> {};
    for (auto const& repo: repos)
    {
        auto requestRows = std::vector<Row> {};
        for (auto const& request: engine.changeRequests(repo))
            requestRows.push_back(pullRequestRow(lists, repo, request));
        appendGroup(pullRequests, engine, std::format("prs/{}", repo), repo, std::move(requestRows));
        auto issueRows = std::vector<Row> {};
        for (auto const& issue: engine.unclaimedIssues(repo))
            issueRows.push_back(issueRow(lists, repo, issue));
        appendGroup(issues, engine, std::format("issues/{}", repo), repo, std::move(issueRows));
    }

    auto rows = std::vector<Row> {};
    appendSection(rows, "MY PULL REQUESTS", "no open pull requests", std::move(pullRequests));
    appendSection(rows, "ISSUES WITHOUT A PR", "no issues without a pull request", std::move(issues));
    appendTimeline(rows, engine);
    appendRunSections(rows, engine, lists);

    // Only while there is one: an orphan is the exception, and an empty section would say nothing.
    auto orphans = std::vector<Row> {};
    for (auto const& repo: repos)
        for (auto const& orphan: engine.orphans(repo))
            orphans.push_back(orphanRow(lists, repo, orphan));
    if (!orphans.empty())
        appendSection(rows, "LEFTOVER WORKTREES", {}, std::move(orphans));
    return rows;
}

namespace
{
    /// @return Whether @p row holds @p filter, ignoring ASCII case: in its text, its repository, or its
    /// group.
    [[nodiscard]] bool matchesFilter(Row const& row, std::string_view filter)
    {
        return containsIgnoringCase(std::format("{} {} {}", row.prefix, row.repo, row.text), filter);
    }
} // namespace

std::vector<std::size_t> visibleRows(std::span<Row const> rows,
                                     std::string_view filter,
                                     Furniture furniture,
                                     ChartRows charts)
{
    auto kept = std::vector<std::size_t> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
    {
        auto const& row = rows[index];
        if (row.isFurniture() && furniture == Furniture::Collapsed)
            continue;
        // A chart that is not drawn takes its heading, and the spacer before that, with it.
        auto const startsChart = [&](std::size_t at) {
            return at < rows.size() && rows[at].kind == RowKind::Chart;
        };
        auto const headsChart = [&](std::size_t at) {
            return at < rows.size() && rows[at].kind == RowKind::Heading && startsChart(at + 1);
        };
        if (charts == ChartRows::Hidden
            && (row.kind == RowKind::Chart || headsChart(index)
                || (row.kind == RowKind::Spacer && headsChart(index + 1))))
            continue;
        // A folded group lists its header only, but while a filter is typed, which looks inside it.
        if (row.kind != RowKind::Group && row.fold == Fold::Folded && filter.empty())
            continue;
        // Under a filter a group's header stays while one of its items does.
        if (row.kind == RowKind::Group && !filter.empty() && !std::ranges::any_of(rows, [&](Row const& item) {
                return item.kind != RowKind::Group && item.group == row.group && matchesFilter(item, filter);
            }))
            continue;
        if (row.isSelectable() && row.kind != RowKind::Group && !filter.empty()
            && !matchesFilter(row, filter))
            continue;
        kept.push_back(index);
    }
    return kept;
}

std::size_t selectableCount(std::span<Row const> rows, std::span<std::size_t const> shown) noexcept
{
    return static_cast<std::size_t>(
        std::ranges::count_if(shown, [rows](std::size_t index) { return rows[index].isSelectable(); }));
}

std::size_t positionOfSelectable(std::span<Row const> rows,
                                 std::span<std::size_t const> shown,
                                 std::size_t index) noexcept
{
    auto seen = std::size_t { 0 };
    for (auto const position: std::views::iota(std::size_t { 0 }, shown.size()))
    {
        if (!rows[shown[position]].isSelectable())
            continue;
        if (seen == index)
            return position;
        ++seen;
    }
    return shown.size();
}

std::vector<std::string> wrap(std::string_view text, int firstColumns, int nextColumns)
{
    auto lines = std::vector<std::string> {};
    if (firstColumns <= 0)
        return lines;
    auto line = std::string {};
    auto used = 0;
    auto const limit = [&] {
        return lines.empty() ? firstColumns : std::max(1, nextColumns);
    };
    auto const endLine = [&] {
        lines.push_back(std::move(line));
        line.clear();
        used = 0;
    };
    for (auto const word: std::views::split(displayText(text, MaxCutChars), ' '))
    {
        auto const wordText = std::string_view { word.begin(), word.end() };
        if (wordText.empty())
            continue;
        auto const width = columnsOf(wordText);
        if (used > 0 && used + 1 + width <= limit())
        {
            line += ' ';
            line += wordText;
            used += 1 + width;
            continue;
        }
        if (used > 0)
            endLine();
        if (width <= limit())
        {
            line = std::string { wordText };
            used = width;
            continue;
        }
        // Wider than a line: in slices, the last of which the next word may join.
        auto slices = slicesOf(wordText, limit(), std::max(1, nextColumns));
        line = std::move(slices.back());
        slices.pop_back();
        std::ranges::move(slices, std::back_inserter(lines));
        used = columnsOf(line);
    }
    if (used > 0)
        endLine();
    return lines;
}

std::vector<std::string> fitted(std::string_view text, DetailFit fit, int firstColumns, int nextColumns)
{
    switch (fit)
    {
        case DetailFit::Wrap: return wrap(text, firstColumns, nextColumns);
        case DetailFit::Cut: return { cut(text, firstColumns) };
        case DetailFit::Aligned: return { cutSanitised(text, firstColumns) };
        case DetailFit::Whole:
            if (firstColumns <= 0)
                return {};
            if (auto line = displayText(text, text.size() + 1); columnsOf(line) <= firstColumns)
                return { std::move(line) };
            return { cut(WidenToSeeText, firstColumns) };
    }
    return {};
}

std::vector<std::string> wrapIndented(std::string_view line, int columns)
{
    auto const rest = line.substr(std::min(line.find_first_not_of(' '), line.size()));
    if (rest.empty() || columns <= 0)
        return { std::string {} };
    // An indentation the line cannot keep beside some text is narrowed to half of it.
    auto const indent = std::min(static_cast<int>(line.size() - rest.size()), columns / 2);
    auto lines = wrap(rest, columns - indent, columns - indent);
    for (auto& piece: lines)
        piece.insert(0, static_cast<std::size_t>(indent), ' ');
    return lines;
}

std::vector<std::string> indentedLines(std::string_view text,
                                       int columns,
                                       std::size_t maxLines,
                                       std::size_t maxLineChars)
{
    auto lines = std::vector<std::string> {};
    if (columns <= 0 || maxLines == 0)
        return lines;
    for (auto const& line: displayLines(text, maxLines, maxLineChars))
        std::ranges::move(wrapIndented(line, columns), std::back_inserter(lines));
    // A cut line, or a wrapped one, never draws past the cap.
    if (lines.size() > maxLines)
    {
        lines.resize(maxLines);
        lines.back() = "…";
    }
    return lines;
}

int columnsOf(std::string_view text)
{
    if (isPrintableAscii(text))
        return static_cast<int>(text.size());
    return core::tui::stringWidth(text);
}

std::string itemLabel(ItemRef const& item)
{
    return std::visit(Overloaded {
                          [](ChangeRequestRef const& ref) { return std::format("#{}", ref.number); },
                          [](IssueRef const& ref) { return std::format("#{}", ref.number); },
                          [](JiraRef const& ref) { return ref.key; },
                          [](TopicRef const&) { return std::string { "topic" }; },
                      },
                      item);
}

void drawFrame(core::tui::Canvas& canvas, std::string_view title)
{
    auto const& border = styleOf(canvas.theme(), Role::Border);
    canvas.drawBox(canvas.area(), core::tui::BorderStyle::Rounded, border);
    // A blank either side of the title, which cut() would trim.
    if (auto const text = cut(title, canvas.width() - 6); !text.empty())
        canvas.putString(0, 2, std::format(" {} ", text), styleOf(canvas.theme(), Role::Text));
}

core::tui::Canvas insideFrame(core::tui::Canvas const& canvas)
{
    return canvas.subcanvas(core::tui::Rect { .x = 1,
                                              .y = 1,
                                              .width = std::max(0, canvas.width() - 2),
                                              .height = std::max(0, canvas.height() - 2) });
}

std::string joinHints(std::span<std::string_view const> hints, int columns)
{
    auto line = std::string {};
    for (auto const hint: hints)
    {
        auto const next =
            line.empty() ? std::string { hint } : std::format("{}{}{}", line, HintSeparator, hint);
        if (columnsOf(next) > columns)
            break;
        line = next;
    }
    return line;
}

std::string cut(std::string_view text, int columns)
{
    return cutSanitised(displayText(text, MaxCutChars), columns);
}

std::string cutSanitised(std::string_view sanitised, int columns)
{
    if (columns <= 0)
        return {};
    if (columnsOf(sanitised) <= columns)
        return std::string { sanitised };
    auto result = std::string {};
    auto used = 0;
    for (auto const& cluster: unicode::utf8_grapheme_segmenter(sanitised))
    {
        auto const width = core::tui::graphemeClusterWidth(cluster);
        if (used + width > columns - 1)
            break;
        for (auto const codepoint: cluster)
            appendUtf8(result, codepoint);
        used += width;
    }
    result += Ellipsis;
    return result;
}

bool containsIgnoringCase(std::string_view haystack, std::string_view needle) noexcept
{
    return !std::ranges::search(haystack, needle, {}, asciiLower, asciiLower).empty();
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, {}, asciiLower, asciiLower);
}

} // namespace orchestrator::view
