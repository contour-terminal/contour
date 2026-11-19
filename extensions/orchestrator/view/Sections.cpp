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
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/core/Sanitize.hpp>
#include <orchestrator/core/Utf8.hpp>

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

    /// Sanitised text for a row or a detail.
    [[nodiscard]] std::string clean(std::string_view text)
    {
        return displayText(text, MaxRowChars);
    }

    // {{{ what each state looks like

    /// How a value is shown: a mark, its role, and a word.
    struct Look
    {
        std::string_view mark = {}; ///< One cell, or empty.
        Role role = Role::Text;     ///< How the mark and the word are drawn.
        std::string_view word = {}; ///< What the row's status or a detail says.
    };

    /// A change request's checks, indexed by CiState.
    constexpr auto CiLooks = std::array {
        Look { .mark = ""sv, .role = Role::Dim, .word = "no checks"sv },
        Look { .mark = "●"sv, .role = Role::Warning, .word = "running"sv },
        Look { .mark = "✓"sv, .role = Role::Passing, .word = "passing"sv },
        Look { .mark = "✗"sv, .role = Role::Failing, .word = "failing"sv },
    };

    /// A change request's review, indexed by ReviewState.
    constexpr auto ReviewLooks = std::array {
        Look { .mark = ""sv, .role = Role::Dim, .word = ""sv },
        Look { .mark = ""sv, .role = Role::Dim, .word = "review required"sv },
        Look { .mark = ""sv, .role = Role::Passing, .word = "approved"sv },
        Look { .mark = ""sv, .role = Role::Warning, .word = "changes requested"sv },
    };

    static_assert(CiLooks.size() == std::to_underlying(CiState::Failing) + 1U);
    static_assert(ReviewLooks.size() == std::to_underlying(ReviewState::ChangesRequested) + 1U);

    /// The entry of @p table that @p value indexes.
    template <typename Entry, std::size_t N, typename Enum>
    [[nodiscard]] constexpr Entry const& entryOf(std::array<Entry, N> const& table, Enum value) noexcept
    {
        auto const index = static_cast<std::size_t>(std::to_underlying(value));
        return table[index < N ? index : 0];
    }

    // }}}
    // {{{ the words a row is made of

    /// @return Such as `#2334`, or a Jira key.

    /// @return Such as `pr-2334`; the run's id when the item has no slug.
    [[nodiscard]] std::string itemName(Run const& run)
    {
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

    // }}}
    // {{{ one row per item

    [[nodiscard]] Row pullRequestRow(Lists const& lists, std::string_view repo, ChangeRequest const& request)
    {
        auto const& ci = entryOf(CiLooks, request.ci);
        auto const& review = entryOf(ReviewLooks, request.review);
        auto details = std::vector<DetailLine> {
            DetailLine { .label = "branch",
                         .value = clean(std::format("{} → {}", request.head, request.base)) },
            DetailLine { .label = "checks", .value = std::string { ci.word }, .role = ci.role },
        };
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
                     .status = std::string { review.word },
                     .statusRole = review.role,
                     .repo = std::string { repo },
                     .item = ChangeRequestRef { request.number },
                     .title = clean(request.title),
                     .details = std::move(details) };
    }

    [[nodiscard]] Row issueRow(Lists const& lists, std::string_view repo, Issue const& issue)
    {
        return Row { .kind = RowKind::Issue,
                     .prefix = prefixOf(lists, repo),
                     .text = clean(std::format("{} {}", itemLabel(issue.item), issue.title)),
                     .repo = std::string { repo },
                     .item = issue.item,
                     .title = clean(issue.title),
                     .details = {
                         DetailLine { .label = "issue", .value = clean(itemLabel(issue.item)) },
                         DetailLine { .label = "url", .value = clean(issue.url), .role = Role::Dim } } };
    }

    /// The details every run row shows.
    [[nodiscard]] std::vector<DetailLine> runDetails(Lists const& lists, Run const& run)
    {
        return { DetailLine { .label = "task", .value = clean(taskLabel(lists.config, run)) },
                 DetailLine { .label = "model", .value = clean(run.model) },
                 DetailLine { .label = "state", .value = clean(runStateText(run)) },
                 DetailLine { .label = "branch", .value = clean(run.branch) },
                 DetailLine { .label = "worktree", .value = clean(run.worktree) },
                 DetailLine { .label = "url", .value = clean(run.url), .role = Role::Dim } };
    }

    [[nodiscard]] Row agentRow(Lists const& lists, Run const& run)
    {
        auto const scope = lists.engine->scopeOf(run);
        auto const look = agentLookOf(run, scope, lists.nowMonoMs);
        auto const age = ageOf(run, scope, lists.nowMonoMs);
        auto const reported = [&](AgentStatus fallback) {
            return agentStatusName(run.lastStatus.value_or(fallback));
        };
        auto [mark, role, status] =
            std::visit(Overloaded {
                           [&](Running const&) {
                               return std::tuple { "○"sv,
                                                   Role::Text,
                                                   std::format("{}{}", reported(AgentStatus::Working), age) };
                           },
                           [&](Blocked const&) {
                               return std::tuple { "●"sv,
                                                   Role::Warning,
                                                   std::format("{}{}", reported(AgentStatus::Blocked), age) };
                           },
                           [](Done const&) {
                               return std::tuple { "●"sv, Role::Passing, std::string { "done · e ends it" } };
                           },
                           [&](auto const&) { return std::tuple { "◌"sv, Role::Dim, runStateText(run) }; },
                       },
                       run.state);
        // An active run outside the configuration stays endable: its cleanup works from what the run stored.
        auto offers = std::vector<KeyCommand> {};
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
        std::ranges::move(runDetails(lists, run), std::back_inserter(details));
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

        auto offers = std::vector<KeyCommand> {};
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

    /// @return Whether @p text is drawn as it is, so that what the user copies is @p text: sanitising it
    ///         changes nothing, and it holds nothing that draws as nothing.
    [[nodiscard]] bool showsAsItIs(std::string_view text)
    {
        // As many characters as it has bytes, so that nothing is cut.
        return displayText(text, text.size() + 1) == text && !holdsInvisible(text);
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
        // The path is the title; what to do about it comes first, so that a short panel still shows it.
        auto details = std::vector<DetailLine> {
            DetailLine { .label = "",
                         .value = "no run accounts for this worktree; it is left as it is",
                         .role = Role::Warning },
            DetailLine { .label = "branch", .value = branch, .fit = DetailFit::Cut },
        };
        if (auto command = removalCommand(lists.engine->mainCheckoutOf(repo), orphan))
            details.push_back(
                DetailLine { .label = "by hand", .value = std::move(*command), .fit = DetailFit::Cut });
        else
            details.push_back(DetailLine { .label = "by hand",
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

    /// @return Whether @p haystack holds @p needle, ignoring ASCII case.
    [[nodiscard]] bool containsIgnoringCase(std::string_view haystack, std::string_view needle) noexcept
    {
        auto const lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return !std::ranges::search(haystack, needle, {}, lower, lower).empty();
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

    auto pullRequests = std::vector<Row> {};
    auto issues = std::vector<Row> {};
    for (auto const& repo: repos)
    {
        for (auto const& request: engine.changeRequests(repo))
            pullRequests.push_back(pullRequestRow(lists, repo, request));
        for (auto const& issue: engine.unclaimedIssues(repo))
            issues.push_back(issueRow(lists, repo, issue));
    }

    auto rows = std::vector<Row> {};
    appendSection(rows, "MY PULL REQUESTS", "no open pull requests", std::move(pullRequests));
    appendSection(rows, "ISSUES WITHOUT A PR", "no issues without a pull request", std::move(issues));
    appendRunSections(rows, engine, lists);

    // Only while there is one: an orphan is the exception, and an empty section would say nothing.
    auto orphans = std::vector<Row> {};
    for (auto const& repo: repos)
        for (auto const& orphan: engine.orphans(repo))
            orphans.push_back(orphanRow(lists, repo, orphan));
    if (!orphans.empty())
        appendSection(rows, "ORPHANS", {}, std::move(orphans));
    return rows;
}

std::vector<std::size_t> visibleRows(std::span<Row const> rows, std::string_view filter, Furniture furniture)
{
    auto kept = std::vector<std::size_t> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
    {
        auto const& row = rows[index];
        if (row.isFurniture() && furniture == Furniture::Collapsed)
            continue;
        if (row.isSelectable() && !filter.empty()
            && !containsIgnoringCase(std::format("{} {}", row.prefix, row.text), filter))
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
        // Wider than a line: broken between clusters, a cluster wider than the line standing alone.
        for (auto const& cluster: unicode::utf8_grapheme_segmenter(wordText))
        {
            auto const clusterWidth = core::tui::graphemeClusterWidth(cluster);
            if (used > 0 && used + clusterWidth > limit())
                endLine();
            for (auto const codepoint: cluster)
                appendUtf8(line, codepoint);
            used += clusterWidth;
        }
    }
    if (used > 0)
        endLine();
    return lines;
}

int columnsOf(std::string_view text)
{
    return core::tui::stringWidth(text);
}

std::string itemLabel(ItemRef const& item)
{
    return std::visit(Overloaded {
                          [](ChangeRequestRef const& ref) { return std::format("#{}", ref.number); },
                          [](IssueRef const& ref) { return std::format("#{}", ref.number); },
                          [](JiraRef const& ref) { return ref.key; },
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
    auto sanitised = displayText(text, MaxCutChars);
    if (columns <= 0)
        return {};
    if (columnsOf(sanitised) <= columns)
        return sanitised;
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

} // namespace orchestrator::view
