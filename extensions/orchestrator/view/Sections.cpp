// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Sections.hpp>

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

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Overloaded.hpp>
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
    [[nodiscard]] std::string itemNumber(ItemRef const& item)
    {
        return std::visit(Overloaded {
                              [](ChangeRequestRef const& ref) { return std::format("#{}", ref.number); },
                              [](IssueRef const& ref) { return std::format("#{}", ref.number); },
                              [](JiraRef const& ref) { return ref.key; },
                          },
                          item);
    }

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

    /// @return Whether the run's row shows its age at @p nowMonoMs: it runs or is blocked, and its age is
    /// known.
    [[nodiscard]] bool showsAge(Run const& run, uint64_t nowMonoMs) noexcept
    {
        auto const ages =
            std::holds_alternative<Running>(run.state) || std::holds_alternative<Blocked>(run.state);
        return ages && run.runningSinceMs != 0 && nowMonoMs >= run.runningSinceMs;
    }

    /// @return The run's age since its tab opened, after a space; empty when that is not known.
    [[nodiscard]] std::string ageOf(Run const& run, uint64_t nowMonoMs)
    {
        if (!showsAge(run, nowMonoMs))
            return {};
        return std::format(" {}", ageText(nowMonoMs - run.runningSinceMs));
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
                     .title = clean(request.title),
                     .details = std::move(details) };
    }

    [[nodiscard]] Row issueRow(Lists const& lists, std::string_view repo, Issue const& issue)
    {
        return Row { .kind = RowKind::Issue,
                     .prefix = prefixOf(lists, repo),
                     .text = clean(std::format("{} {}", itemNumber(issue.item), issue.title)),
                     .title = clean(issue.title),
                     .details = {
                         DetailLine { .label = "issue", .value = clean(itemNumber(issue.item)) },
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
        auto const age = ageOf(run, lists.nowMonoMs);
        auto const reported = [&](AgentStatus fallback) {
            return agentStatusName(run.lastStatus.value_or(fallback));
        };
        auto const [mark, role, status] =
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
        auto offers = std::vector<KeyCommand> {};
        if (countsAsActive(run))
            offers.push_back(KeyCommand::EndRun);
        auto const text = std::format("{} · {} · {}", itemName(run), taskLabel(lists.config, run), run.model);
        return Row { .kind = RowKind::Agent,
                     .mark = std::string { mark },
                     .markRole = role,
                     .prefix = prefixOf(lists, run.repo),
                     .text = clean(text),
                     .status = clean(status),
                     .statusRole = role == Role::Text ? Role::Dim : role,
                     .run = run.id,
                     .offers = std::move(offers),
                     .title = clean(text),
                     .details = runDetails(lists, run) };
    }

    [[nodiscard]] Row finishedRow(Lists const& lists, Run const& run, Finished const& finished)
    {
        auto const isChecking = !std::holds_alternative<Finished>(run.state);
        auto const kept = finished.fate == Fate::Kept;
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
        if (!isChecking && kept)
            offers.push_back(KeyCommand::RetryRemoval);
        if (!isChecking)
            offers.push_back(KeyCommand::Dismiss);

        auto details = runDetails(lists, run);
        if (run.failure)
            details.push_back(
                DetailLine { .label = "failure", .value = clean(run.failure->why), .role = Role::Failing });
        for (auto const& entry: finished.entries)
            details.push_back(DetailLine { .label = "", .value = clean(entry), .role = Role::Warning });
        if (!isChecking && kept)
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
} // namespace

std::optional<uint64_t> nextAgeChange(Engine const& engine, uint64_t nowMonoMs)
{
    auto earliest = std::optional<uint64_t> {};
    for (auto const& run: engine.runs())
    {
        if (!showsAge(run, nowMonoMs))
            continue;
        auto const age = nowMonoMs - run.runningSinceMs;
        auto const unitMs = ageUnitOf(age).unitMs;
        auto const changesAt = run.runningSinceMs + (((age / unitMs) + 1) * unitMs);
        earliest = earliest ? std::min(*earliest, changesAt) : changesAt;
    }
    return earliest;
}

std::vector<Row> buildRows(Engine const& engine, uint64_t nowMonoMs)
{
    auto const* const config = engine.config();
    if (config == nullptr)
        return {
            Row { .kind = RowKind::Notice, .text = "Nothing to list until the orchestrator is configured." },
            Row { .kind = RowKind::Notice, .text = "Configure daemon.extensions.orchestrator.config:" },
            Row { .kind = RowKind::Notice, .text = "worktree_root, models, repos and tasks." },
            Row { .kind = RowKind::Notice, .text = "The line above says what is missing or wrong." },
        };

    auto const repos = engine.repositories();
    auto const lists = Lists { .config = config,
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

    auto agents = std::vector<Row> {};
    auto finished = std::vector<Row> {};
    for (auto const& run: engine.runs())
    {
        if (auto const* const entry = finishedEntryOf(run))
            finished.push_back(finishedRow(lists, run, *entry));
        else
            agents.push_back(agentRow(lists, run));
    }

    auto rows = std::vector<Row> {};
    appendSection(rows, "MY PULL REQUESTS", "no open pull requests", std::move(pullRequests));
    appendSection(rows, "ISSUES WITHOUT A PR", "no issues without a pull request", std::move(issues));
    appendSection(rows,
                  std::format("AGENTS · {} OF {} RUNNING",
                              std::ranges::count_if(engine.runs(), countsAsActive),
                              config->maxAgents()),
                  "no agents",
                  std::move(agents));
    appendSection(rows,
                  std::format("FINISHED · {} NEW", newFinishedCount(engine.runs())),
                  "nothing finished",
                  std::move(finished));
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

int columnsOf(std::string_view text)
{
    return core::tui::stringWidth(text);
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
