// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/WorkItems.hpp>

#include <algorithm>
#include <charconv>
#include <compare>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/TaskRoles.hpp>

namespace orchestrator
{

namespace
{

    using namespace std::string_view_literals;

    // {{{ characters

    [[nodiscard]] constexpr bool isUpper(char c) noexcept
    {
        return c >= 'A' && c <= 'Z';
    }

    [[nodiscard]] constexpr bool isLower(char c) noexcept
    {
        return c >= 'a' && c <= 'z';
    }

    [[nodiscard]] constexpr bool isDigit(char c) noexcept
    {
        return c >= '0' && c <= '9';
    }

    [[nodiscard]] constexpr bool isLetter(char c) noexcept
    {
        return isUpper(c) || isLower(c);
    }

    [[nodiscard]] constexpr bool isWordByte(char c) noexcept
    {
        return isLetter(c) || isDigit(c);
    }

    [[nodiscard]] constexpr bool isBlank(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    [[nodiscard]] constexpr char upperOf(char c) noexcept
    {
        return isLower(c) ? static_cast<char>(c - 'a' + 'A') : c;
    }

    [[nodiscard]] constexpr char lowerOf(char c) noexcept
    {
        return isUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
    }

    [[nodiscard]] std::string upperCased(std::string_view text)
    {
        auto result = std::string(text);
        std::ranges::transform(result, result.begin(), upperOf);
        return result;
    }

    [[nodiscard]] std::string lowerCased(std::string_view text)
    {
        auto result = std::string(text);
        std::ranges::transform(result, result.begin(), lowerOf);
        return result;
    }

    /// @return How many leading bytes of @p text satisfy @p pred.
    template <typename Pred>
    [[nodiscard]] std::size_t prefixLength(std::string_view text, Pred pred) noexcept
    {
        return static_cast<std::size_t>(std::ranges::find_if_not(text, pred) - text.begin());
    }

    /// @return Whether a word may start at @p at: it is the first byte, or follows a byte that is no word
    /// byte.
    [[nodiscard]] bool atWordStart(std::string_view text, std::size_t at) noexcept
    {
        return at == 0 || !isWordByte(text[at - 1]);
    }

    /// @return Whether a word ends at @p at: it is the end, or a byte that is no word byte.
    [[nodiscard]] bool atWordEnd(std::string_view text, std::size_t at) noexcept
    {
        return at >= text.size() || !isWordByte(text[at]);
    }

    [[nodiscard]] std::string_view skipBlanks(std::string_view text) noexcept
    {
        return text.substr(prefixLength(text, isBlank));
    }

    [[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
    {
        text = skipBlanks(text);
        auto const last = std::ranges::find_if_not(text | std::views::reverse, isBlank);
        return text.substr(0, static_cast<std::size_t>(last.base() - text.begin()));
    }

    /// @return Whether @p text starts with the word @p word, case-insensitively, at a word boundary on both
    /// sides.
    [[nodiscard]] bool startsWithWord(std::string_view text, std::size_t at, std::string_view word) noexcept
    {
        if (!atWordStart(text, at) || text.size() - at < word.size())
            return false;
        auto const candidate = text.substr(at, word.size());
        return std::ranges::equal(candidate, word, {}, lowerOf, lowerOf) && atWordEnd(text, at + word.size());
    }

    [[nodiscard]] std::optional<uint64_t> numberOf(std::string_view digits) noexcept
    {
        auto value = uint64_t { 0 };
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc {} || end != digits.data() + digits.size())
            return std::nullopt;
        return value;
    }

    // }}}
    // {{{ Jira keys

    /// How a text spells the keys in it.
    enum class KeyCase : uint8_t
    {
        Exact = 0, ///< Upper case only: a title.
        Folded,    ///< Either case, upper-cased once found: a branch name.
    };

    /// @return The project part of @p key: what precedes its `-`.
    [[nodiscard]] std::string_view projectOf(std::string_view key) noexcept
    {
        return key.substr(0, key.find('-'));
    }

    /// @return The Jira key whose token starts at @p at in @p text, upper-cased; std::nullopt: no key starts
    /// there.
    [[nodiscard]] std::optional<std::string> keyAt(std::string_view text, std::size_t at, KeyCase keyCase)
    {
        auto const startsProject = keyCase == KeyCase::Exact ? isUpper : isLetter;
        if (!startsProject(text[at]) || !atWordStart(text, at))
            return std::nullopt;
        auto const inProject = [keyCase](char c) {
            return keyCase == KeyCase::Exact ? isUpper(c) || isDigit(c) : isWordByte(c);
        };
        auto const rest = text.substr(at);
        auto const projectLength = 1 + prefixLength(rest.substr(1), inProject);
        if (projectLength < 2 || projectLength >= rest.size() || rest[projectLength] != '-')
            return std::nullopt;
        auto const digitsLength = prefixLength(rest.substr(projectLength + 1), isDigit);
        auto const keyLength = projectLength + 1 + digitsLength;
        if (digitsLength == 0 || !atWordEnd(rest, keyLength))
            return std::nullopt;
        auto key = upperCased(rest.substr(0, keyLength));
        return isJiraKey(key) ? std::optional { std::move(key) } : std::nullopt;
    }

    /// @return The first key in @p text whose project @p projects holds.
    [[nodiscard]] std::optional<std::string> firstCoveredKey(std::string_view text,
                                                             KeyCase keyCase,
                                                             std::span<std::string const> projects)
    {
        for (auto const at: std::views::iota(std::size_t { 0 }, text.size()))
            if (auto key = keyAt(text, at, keyCase); key && std::ranges::contains(projects, projectOf(*key)))
                return key;
        return std::nullopt;
    }

    // }}}
    // {{{ JQL

    /// @return @p name without blanks around it and without the quotes around it.
    [[nodiscard]] std::string_view unquoted(std::string_view name) noexcept
    {
        name = trimmed(name);
        if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front())
            return name.substr(1, name.size() - 2);
        return name;
    }

    /// Appends @p name, upper-cased, when it can be a key's project part.
    void appendProject(std::string_view name, std::vector<std::string>& projects)
    {
        auto project = upperCased(unquoted(name));
        if (isJiraKey(project + "-1"))
            projects.push_back(std::move(project));
    }

    /// @return The one name after `project =`: quoted, or up to a blank or a JQL delimiter.
    [[nodiscard]] std::string_view firstName(std::string_view text) noexcept
    {
        if (!text.empty() && (text.front() == '"' || text.front() == '\''))
        {
            auto const close = text.find(text.front(), 1);
            return close == std::string_view::npos ? text.substr(1) : text.substr(1, close - 1);
        }
        return text.substr(0, prefixLength(text, [](char c) { return !isBlank(c) && !"(),"sv.contains(c); }));
    }

    /// Appends every project @p jql names: `project = <name>` and `project in (<name>, …)`,
    /// case-insensitively.
    void appendJqlProjects(std::string_view jql, std::vector<std::string>& projects)
    {
        constexpr auto Field = "project"sv;
        constexpr auto In = "in"sv;
        for (auto const at: std::views::iota(std::size_t { 0 }, jql.size()))
        {
            if (!startsWithWord(jql, at, Field))
                continue;
            auto const rest = skipBlanks(jql.substr(at + Field.size()));
            if (rest.starts_with('='))
                appendProject(firstName(skipBlanks(rest.substr(1))), projects);
            else if (startsWithWord(rest, 0, In))
            {
                auto const list = skipBlanks(rest.substr(In.size()));
                if (!list.starts_with('('))
                    continue;
                auto const close = list.find(')');
                if (close == std::string_view::npos)
                    continue;
                for (auto const name: list.substr(1, close - 1) | std::views::split(',')
                                          | std::views::transform([](auto const& part) {
                                                return std::string_view(part.begin(), part.end());
                                            }))
                    appendProject(name, projects);
            }
        }
    }

    // }}}
    // {{{ joining

    /// @return The active run of @p repo on @p item, if one does.
    [[nodiscard]] Run const* activeRunOn(std::span<Run const> runs,
                                         std::string_view repo,
                                         ItemRef const& item)
    {
        auto const found = std::ranges::find_if(runs, [&](Run const& run) {
            return countsAsActive(run) && run.repo == repo && run.item == item;
        });
        return found != runs.end() ? &*found : nullptr;
    }

    /// Credits @p item with the active run on its item, else on @p linked.
    void attachAgent(WorkItem& item, std::span<Run const> runs, std::optional<ItemRef> const& linked)
    {
        auto const* run = activeRunOn(runs, item.repo, item.item);
        if (run == nullptr && linked)
            run = activeRunOn(runs, item.repo, *linked);
        if (run == nullptr)
            return;
        item.agentRun = run->id;
        item.agentWorker = std::string(workerIdOf(*run));
    }

    [[nodiscard]] std::string forgeKeyOf(uint64_t number)
    {
        return "#" + std::to_string(number);
    }

    /// @return The identity a ticket is de-duplicated by: its Jira key, or `<repo>/#<n>` for a forge issue;
    ///         std::nullopt for an item that is no ticket.
    [[nodiscard]] std::optional<std::string> ticketIdentity(std::string_view repo, ItemRef const& item)
    {
        return std::visit(Overloaded {
                              [](JiraRef const& ref) { return std::optional { ref.key }; },
                              [&](IssueRef const& ref) {
                                  return std::optional { std::string(repo) + "/" + forgeKeyOf(ref.number) };
                              },
                              [](auto const&) { return std::optional<std::string> {}; },
                          },
                          item);
    }

    /// The tickets an MR row looks its linked ticket up in.
    struct TicketLookup
    {
        std::span<RepoWork const> repos;
        std::span<Issue const> linkedTickets;

        /// @return The ticket @p key: listed by any repository first, else looked up.
        [[nodiscard]] Issue const* jira(std::string_view key) const noexcept
        {
            auto const matches = [key](Issue const& issue) {
                auto const* const ref = std::get_if<JiraRef>(&issue.item);
                return ref != nullptr && ref->key == key;
            };
            for (auto const& repo: repos)
                if (auto const found = std::ranges::find_if(repo.issues, matches, &ListedIssue::issue);
                    found != repo.issues.end())
                    return &found->issue;
            auto const found = std::ranges::find_if(linkedTickets, matches);
            return found != linkedTickets.end() ? &*found : nullptr;
        }

        /// @return The forge issue @p number as @p repo listed it.
        [[nodiscard]] static Issue const* forge(RepoWork const& repo, uint64_t number) noexcept
        {
            auto const found = std::ranges::find_if(repo.issues, [number](ListedIssue const& listed) {
                auto const* const ref = std::get_if<IssueRef>(&listed.issue.item);
                return ref != nullptr && ref->number == number;
            });
            return found != repo.issues.end() ? &found->issue : nullptr;
        }
    };

    /// @return An MR row; adds the tickets it names to @p named.
    [[nodiscard]] WorkItem mrRowOf(RepoWork const& repo,
                                   ChangeRequest const& mr,
                                   WorkInput const& input,
                                   std::vector<std::string>& named)
    {
        auto const lookup = TicketLookup { .repos = input.repos, .linkedTickets = input.linkedTickets };
        auto item = WorkItem { .group = groupOf(mr),
                               .repo = std::string(repo.repo),
                               .changeRequest = mr,
                               .ticket = std::nullopt,
                               .ticketKey = {},
                               .item = ChangeRequestRef { mr.number },
                               .updatedWallMs = mr.updatedWallMs,
                               .agentRun = {},
                               .agentWorker = {} };
        auto const forgeIssues = namedForgeIssues(mr);
        for (auto const number: forgeIssues)
            named.push_back(*ticketIdentity(repo.repo, IssueRef { number }));

        auto linked = std::optional<ItemRef> {};
        if (auto key = linkedJiraKey(mr.title, mr.head, input.jiraProjects))
        {
            if (auto const* const ticket = lookup.jira(*key))
                item.ticket = *ticket;
            named.push_back(*key);
            linked = JiraRef { *key };
            item.ticketKey = std::move(*key);
        }
        else if (!forgeIssues.empty())
        {
            auto const number = forgeIssues.front();
            if (auto const* const ticket = TicketLookup::forge(repo, number))
                item.ticket = *ticket;
            linked = IssueRef { number };
            item.ticketKey = forgeKeyOf(number);
        }
        if (item.updatedWallMs == 0 && item.ticket)
            item.updatedWallMs = item.ticket->updatedWallMs;
        attachAgent(item, input.runs, linked);
        return item;
    }

    // }}}
    // {{{ ordering

    /// Whether an item's update time is known.
    enum class Dating : uint8_t
    {
        Known = 0, ///< Ordered by its time, newest first.
        Unknown,   ///< Ordered by its position in its source's result.
    };

    /// Whether a row is an MR's or a ticket's.
    enum class RowKind : uint8_t
    {
        ChangeRequest = 0, ///< An MR row.
        Ticket,            ///< A ticket row.
    };

    /// What orderOf() compares, most significant first.
    struct SortKey
    {
        WorkGroup group = WorkGroup::NeedsYou; ///< The group; one for all under WorkSort::LastUpdate.
        Dating dating = Dating::Known;
        uint64_t age = 0;     ///< Newest first: the time's distance from the largest; 0 when unknown.
        std::size_t rank = 0; ///< The ticket's Issue::listRank when its time is unknown; else 0.
        std::size_t repo = 0; ///< The repository's position in the configuration.
        RowKind kind = RowKind::ChangeRequest;
        std::string_view project; ///< A Jira key's project part; empty for an MR or a forge issue.
        uint64_t number = 0;      ///< The MR's or issue's number, or the Jira key's.
        [[nodiscard]] auto operator<=>(SortKey const&) const = default;
    };

    [[nodiscard]] SortKey sortKeyOf(WorkItem const& item,
                                    std::span<std::string const> repoOrder,
                                    WorkSort sort)
    {
        auto const dating = item.updatedWallMs != 0 ? Dating::Known : Dating::Unknown;
        auto const isTicketRow = !item.changeRequest.has_value();
        auto key = SortKey {
            .group = sort == WorkSort::Importance ? item.group : WorkGroup::NeedsYou,
            .dating = dating,
            .age = dating == Dating::Known ? std::numeric_limits<uint64_t>::max() - item.updatedWallMs : 0,
            .rank = dating == Dating::Unknown && isTicketRow && item.ticket ? item.ticket->listRank : 0,
            .repo = static_cast<std::size_t>(std::ranges::find(repoOrder, item.repo) - repoOrder.begin()),
            .kind = isTicketRow ? RowKind::Ticket : RowKind::ChangeRequest,
            .project = {},
            .number = 0,
        };
        std::visit(Overloaded {
                       [&](ChangeRequestRef const& ref) { key.number = ref.number; },
                       [&](IssueRef const& ref) { key.number = ref.number; },
                       [&](JiraRef const& ref) {
                           key.project = projectOf(ref.key);
                           key.number =
                               numberOf(std::string_view(ref.key).substr(key.project.size() + 1)).value_or(0);
                       },
                       [&](TopicRef const& ref) { key.project = ref.slug; },
                   },
                   item.item);
        return key;
    }

    // }}}

} // namespace

std::string_view workerIdOf(Run const& run) noexcept
{
    return run.worker.empty() ? ImplicitWorkerId : std::string_view { run.worker };
}

std::string workItemKey(WorkItem const& item)
{
    auto const own =
        std::visit(Overloaded {
                       [](ChangeRequestRef const& ref) { return "!" + std::to_string(ref.number); },
                       [](IssueRef const& ref) { return forgeKeyOf(ref.number); },
                       [](JiraRef const& ref) { return ref.key; },
                       [](TopicRef const& ref) { return ref.slug; },
                   },
                   item.item);
    return item.repo + "/" + own;
}

std::vector<std::string> jiraProjectsOf(std::span<Repo const> repos, std::span<RepoWork const> listed)
{
    auto projects = std::vector<std::string> {};
    for (auto const& repo: listed)
        for (auto const& entry: repo.issues)
            if (auto const* const ref = std::get_if<JiraRef>(&entry.issue.item))
                projects.emplace_back(projectOf(ref->key));
    constexpr auto JqlKey = "jql"sv;
    for (auto const& repo: repos)
        for (auto const& query: repo.issueQueries)
            if (auto const* const source = findIssueSource(issueSources(), query.source);
                source != nullptr && source->configKey == JqlKey)
                appendJqlProjects(query.text, projects);
    std::ranges::sort(projects);
    auto const [first, last] = std::ranges::unique(projects);
    projects.erase(first, last);
    return projects;
}

std::optional<std::string> linkedJiraKey(std::string_view title,
                                         std::string_view branch,
                                         std::span<std::string const> projects)
{
    if (auto key = firstCoveredKey(title, KeyCase::Exact, projects))
        return key;
    return firstCoveredKey(branch, KeyCase::Folded, projects);
}

std::vector<uint64_t> namedForgeIssues(ChangeRequest const& mr)
{
    auto numbers = std::vector<uint64_t> {};
    auto const add = [&](uint64_t number) {
        if (!std::ranges::contains(numbers, number))
            numbers.push_back(number);
    };
    for (auto const number: mr.closesIssues)
        add(number);
    auto const title = std::string_view { mr.title };
    for (auto const at: std::views::iota(std::size_t { 0 }, title.size()))
    {
        if (title[at] != '#' || !atWordStart(title, at))
            continue;
        auto const digits = title.substr(at + 1, prefixLength(title.substr(at + 1), isDigit));
        if (digits.empty() || !atWordEnd(title, at + 1 + digits.size()))
            continue;
        if (auto const number = numberOf(digits))
            add(*number);
    }
    return numbers;
}

Progress progressOf(Issue const& issue, std::optional<std::span<std::string const>> inProgressKeys) noexcept
{
    auto const inProgress = std::visit(Overloaded {
                                           [](IssueRef const&) { return true; },
                                           [&](JiraRef const& ref) {
                                               return inProgressKeys
                                                          ? std::ranges::contains(*inProgressKeys, ref.key)
                                                          : issue.category == StatusCategory::InProgress;
                                           },
                                           [](auto const&) { return false; },
                                       },
                                       issue.item);
    return inProgress ? Progress::InProgress : Progress::NotStarted;
}

WorkGroup groupOf(ChangeRequest const& mr) noexcept
{
    if (mr.readiness == Readiness::Draft)
        return WorkGroup::EverythingElse;
    if (mr.ci == CiState::Failing || mr.review == ReviewState::ChangesRequested
        || mr.conflicts == Conflicts::Conflicting)
        return WorkGroup::NeedsYou;
    // An inferred MR's ci None and review None say nothing: only GitLab's `mergeable` (Approved) means ready.
    auto const ready = mr.source == StateSource::Inferred
                           ? mr.review == ReviewState::Approved
                           : (mr.ci == CiState::Passing || mr.ci == CiState::None)
                                 && (mr.review == ReviewState::Approved || mr.review == ReviewState::None);
    return ready ? WorkGroup::ReadyToMerge : WorkGroup::InReview;
}

WorkGroup groupOf(Progress progress) noexcept
{
    return progress == Progress::InProgress ? WorkGroup::NoMrYet : WorkGroup::EverythingElse;
}

std::vector<WorkItem> workItems(WorkInput const& input)
{
    auto items = std::vector<WorkItem> {};
    auto taken = std::vector<std::string> {}; // ticketIdentity() of every ticket named or shown
    for (auto const& repo: input.repos)
        for (auto const& mr: repo.changeRequests)
            items.push_back(mrRowOf(repo, mr, input, taken));

    for (auto const& repo: input.repos)
        for (auto const& listed: repo.issues)
        {
            auto identity = ticketIdentity(repo.repo, listed.issue.item);
            if (!identity || std::ranges::contains(taken, *identity))
                continue;
            taken.push_back(std::move(*identity));
            auto const* const jira = std::get_if<JiraRef>(&listed.issue.item);
            auto item = WorkItem {
                .group = groupOf(listed.progress),
                .repo = std::string(repo.repo),
                .changeRequest = std::nullopt,
                .ticket = listed.issue,
                .ticketKey =
                    jira != nullptr ? jira->key : forgeKeyOf(std::get<IssueRef>(listed.issue.item).number),
                .item = listed.issue.item,
                .updatedWallMs = listed.issue.updatedWallMs,
                .agentRun = {},
                .agentWorker = {},
            };
            attachAgent(item, input.runs, std::nullopt);
            items.push_back(std::move(item));
        }
    return items;
}

std::vector<std::size_t> orderOf(std::span<WorkItem const> items,
                                 std::span<std::string const> repoOrder,
                                 WorkSort sort)
{
    auto keys = std::vector<SortKey> {};
    keys.reserve(items.size());
    for (auto const& item: items)
        keys.push_back(sortKeyOf(item, repoOrder, sort));
    auto order = std::views::iota(std::size_t { 0 }, items.size()) | std::ranges::to<std::vector>();
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
    return order;
}

bool matchesFilter(WorkItem const& item, std::string_view filter)
{
    if (filter.empty())
        return true;
    auto const folded = lowerCased(trimmed(filter));
    if (item.changeRequest)
    {
        auto const number =
            std::string_view { folded }.substr(folded.starts_with('!') || folded.starts_with('#') ? 1 : 0);
        if (number == std::to_string(item.changeRequest->number))
            return true;
    }
    auto fields = std::vector<std::string_view> { item.ticketKey, item.repo };
    if (item.changeRequest)
        fields.insert(fields.end(), { item.changeRequest->title, item.changeRequest->url });
    if (item.ticket)
        fields.insert(fields.end(), { item.ticket->title, item.ticket->url });
    return std::ranges::any_of(fields,
                               [&](std::string_view field) { return lowerCased(field).contains(folded); });
}

namespace
{

    /// @return The first task of @p config that has @p role and applies to @p kind; nullptr: none.
    [[nodiscard]] TaskDef const* taskWithRole(Config const& config, TaskRole role, AppliesTo kind) noexcept
    {
        auto const tasks = config.tasks();
        auto const found = std::ranges::find_if(
            tasks, [&](TaskDef const& task) { return task.role == role && task.appliesTo == kind; });
        return found != tasks.end() ? &*found : nullptr;
    }

    /// @return The reason a failing merge request's suggestion gives: its failing checks, `, `-joined.
    [[nodiscard]] std::string failingReason(ChangeRequest const& mr)
    {
        auto reason = std::string { "pipeline fails" };
        auto separator = std::string_view { ": " };
        for (auto const& check: mr.failingChecks)
        {
            reason += separator;
            reason += check;
            separator = ", ";
        }
        return reason;
    }

} // namespace

TaskSuggestion suggestTask(Config const& config, WorkItem const& item)
{
    auto const kind = appliesToOf(item.item);
    if (item.changeRequest)
    {
        auto const& mr = *item.changeRequest;
        if (mr.ci == CiState::Failing)
        {
            if (auto const* const task = taskWithRole(config, TaskRole::FixCi, kind))
                return { .task = task, .reason = failingReason(mr) };
        }
        if (mr.conflicts == Conflicts::Conflicting)
        {
            if (auto const* const task = taskWithRole(config, TaskRole::ResolveConflicts, kind))
                return { .task = task, .reason = "conflicts with " + mr.base };
        }
        if (mr.review == ReviewState::ChangesRequested)
        {
            if (auto const* const task = taskWithRole(config, TaskRole::AddressReview, kind))
                return { .task = task, .reason = "changes requested" };
        }
    }
    else if (auto const* const task = taskWithRole(config, TaskRole::WorkOnIssue, kind))
    {
        return { .task = task, .reason = item.group == WorkGroup::NoMrYet ? "ticket in progress" : "ticket" };
    }
    return { .task = config.firstTaskFor(kind),
             .reason = item.changeRequest ? "first task for a merge request" : "first task for a ticket" };
}

std::size_t activeRunsOn(Config const& /*config*/,
                         std::span<Run const> runs,
                         std::string_view workerId) noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(
        runs, [&](Run const& run) { return countsAsActive(run) && workerIdOf(run) == workerId; }));
}

WorkerDef const& defaultWorker(Config const& config, std::span<Run const> runs, std::string_view taskId)
{
    auto const& own = config.workerFor(taskId);
    if (activeRunsOn(config, runs, own.id) == 0)
        return own;
    auto const workers = config.workers();
    return *std::ranges::min_element(
        workers, {}, [&](WorkerDef const& worker) { return activeRunsOn(config, runs, worker.id); });
}

} // namespace orchestrator
