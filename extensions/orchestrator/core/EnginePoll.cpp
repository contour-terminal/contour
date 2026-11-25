// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Agents.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/EngineDetail.hpp>
#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Plugins.hpp>
#include <orchestrator/core/Provider.hpp>
#include <orchestrator/core/Sanitize.hpp>
#include <orchestrator/core/WorkItems.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    using engine::detail::BoardTagSuffix;
    using engine::detail::BoardViewTagSuffix;
    using engine::detail::inWorktree;
    using engine::detail::judge;
    using engine::detail::Judgement;
    using engine::detail::later;
    using engine::detail::PollTagPrefix;
    using engine::detail::Verdict;

    constexpr auto AuthTag = "poll/auth"sv;
    constexpr auto HooksTag = "poll/hooks"sv;
    constexpr auto HooksCommand = "agent-setup-check"sv;
    constexpr auto PluginsTag = "poll/plugins"sv;
    constexpr auto PluginsCommand = "claude-plugins"sv;
    constexpr auto PluginStepTag = "poll/plugin-step"sv;
    constexpr auto MarketplaceAddCommand = "claude-marketplace-add"sv;
    constexpr auto PluginInstallCommand = "claude-plugin-install"sv;
    constexpr auto WorktreeListCommand = "git-worktree-list"sv;

    /// What a repository's poll tag names after `poll/<repo>/`.
    constexpr auto PrsList = "prs"sv;
    constexpr auto IssuesList = "issues/"sv;
    constexpr auto DefaultBranchList = "default-branch"sv;
    constexpr auto WorktreesList = "worktrees"sv;
    constexpr auto PipelinesList = "pipelines"sv;
    constexpr auto FailedJobsList = "failed-jobs"sv;
    constexpr auto ProgressList = "progress/"sv;
    constexpr auto LinkedList = "jira-keys"sv;

    /// What a repository's poll lists.
    enum class PollList : uint8_t
    {
        ChangeRequests, ///< Its open change requests.
        Issues,         ///< One issue query's issues.
        DefaultBranch,  ///< Its default branch.
        Worktrees,      ///< Its worktrees.
        Pipelines,      ///< Its newest pipelines.
        FailedJobs,     ///< Its failed jobs.
        Progress,       ///< One issue query's tickets in progress.
        Linked,         ///< The key lookup: the tickets merge requests name that no source listed.
    };

    /// Whether a list's name is followed by the index of the issue query it is for.
    enum class ListIndex : uint8_t
    {
        None = 0, ///< The name is the whole list.
        Query,    ///< `<name><query index>`.
    };

    /// One list a repository's poll tag may name.
    struct PollListRow
    {
        PollList list;         ///< The list.
        std::string_view name; ///< Its name in the tag.
        ListIndex index;       ///< Whether the name is followed by a query's index.
    };

    /// Every list, one row each: the parser looks a tag's list up here.
    constexpr auto PollListRows = std::array {
        PollListRow { .list = PollList::ChangeRequests, .name = PrsList, .index = ListIndex::None },
        PollListRow { .list = PollList::Issues, .name = IssuesList, .index = ListIndex::Query },
        PollListRow { .list = PollList::DefaultBranch, .name = DefaultBranchList, .index = ListIndex::None },
        PollListRow { .list = PollList::Worktrees, .name = WorktreesList, .index = ListIndex::None },
        PollListRow { .list = PollList::Pipelines, .name = PipelinesList, .index = ListIndex::None },
        PollListRow { .list = PollList::FailedJobs, .name = FailedJobsList, .index = ListIndex::None },
        PollListRow { .list = PollList::Progress, .name = ProgressList, .index = ListIndex::Query },
        PollListRow { .list = PollList::Linked, .name = LinkedList, .index = ListIndex::None },
    };

    /// The configuration key of a source whose query is JQL: the one a key lookup searches.
    constexpr auto JqlKey = "jql"sv;

    /// What separates a set's or an issue source's name from its check in the check's tag. No repository
    /// name holds it (an ArgumentKind::Name), so `poll/hooks:<set>` is never `poll/<repo>/<list>` of a
    /// repository `hooks`, nor `poll/auth:<source>` one of a repository `auth`.
    constexpr auto SetTagSeparator = ':';

    /// @param check HooksTag or PluginsTag.
    /// @param envSet A variable set's name; "" for none.
    /// @return The tag of @p check for @p envSet: @p check itself for none, else `<check>:<set>`.
    [[nodiscard]] std::string setupTagOf(std::string_view check, std::string_view envSet)
    {
        return envSet.empty() ? std::string(check) : std::format("{}{}{}", check, SetTagSeparator, envSet);
    }

    /// @param tag A poll tag.
    /// @param check HooksTag, PluginsTag or AuthTag.
    /// @return The set @p tag is @p check's tag for ("" for none), or std::nullopt when it is not one.
    [[nodiscard]] std::optional<std::string_view> setOfSetupTag(std::string_view tag,
                                                                std::string_view check) noexcept
    {
        if (tag == check)
            return std::string_view {};
        if (tag.starts_with(check) && tag.substr(check.size()).starts_with(SetTagSeparator))
            return tag.substr(check.size() + 1);
        return std::nullopt;
    }

    /// @return Whether @p tag is a hooks or plugins check's, of any set.
    [[nodiscard]] bool isSetCheckTag(std::string_view tag) noexcept
    {
        return setOfSetupTag(tag, HooksTag) || setOfSetupTag(tag, PluginsTag);
    }

    /// @return @p arguments, followed by @p envSet's argument when it names a set.
    [[nodiscard]] std::vector<Argument> withSet(std::vector<Argument> arguments, std::string_view envSet)
    {
        if (!envSet.empty())
            arguments.push_back(Argument { .name = EnvSetArgument, .value = std::string(envSet) });
        return arguments;
    }

    /// @return How a line says whose check or install it was: "" for none, else ` for worker <a>`.
    [[nodiscard]] std::string forWorkers(std::span<std::string const> workers)
    {
        return workers.empty() ? std::string {} : std::format(" for {}", workersNamed(workers));
    }

    /// A repository's poll tag, taken apart.
    struct PollTag
    {
        std::string_view repo; ///< The repository's name.
        PollList list;         ///< What was listed.
        std::size_t query = 0; ///< The issue query it is for (ListIndex::Query); 0 otherwise.
    };

    /// @return The index @p digits spell whole; std::nullopt when they spell none.
    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view digits) noexcept
    {
        auto index = std::size_t { 0 };
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
        if (error != std::errc {} || end != digits.data() + digits.size())
            return std::nullopt;
        return index;
    }

    /// @return @p list, looked up in PollListRows, as a tag of @p repo; std::nullopt when no row names it.
    [[nodiscard]] std::optional<PollTag> parseList(std::string_view repo, std::string_view list) noexcept
    {
        for (auto const& row: PollListRows)
            switch (row.index)
            {
                case ListIndex::None:
                    if (list == row.name)
                        return PollTag { .repo = repo, .list = row.list, .query = 0 };
                    break;
                case ListIndex::Query:
                    if (list.starts_with(row.name))
                        if (auto const query = indexOf(list.substr(row.name.size())))
                            return PollTag { .repo = repo, .list = row.list, .query = *query };
                    break;
            }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<PollTag> parsePollTag(std::string_view tag) noexcept
    {
        if (!tag.starts_with(PollTagPrefix))
            return std::nullopt;
        auto const rest = tag.substr(PollTagPrefix.size());
        auto const slash = rest.find('/');
        if (slash == std::string_view::npos)
            return std::nullopt;
        return parseList(rest.substr(0, slash), rest.substr(slash + 1));
    }

    /// @return How much of a poll command's output may be read: all of it, what came before the host's cut,
    ///         or nothing (std::nullopt: it failed).
    [[nodiscard]] std::optional<OutputExtent> readableExtent(Judgement const& judgement) noexcept
    {
        switch (judgement.verdict)
        {
            case Verdict::Ok: return OutputExtent::Whole;
            case Verdict::Cut: return OutputExtent::Truncated;
            default: return std::nullopt;
        }
    }

    /// @param map A provider row's mapping; nullptr: none.
    /// @param outcome How its command ended.
    /// @return What @p map read from @p outcome; std::nullopt when the command failed or @p map refused it.
    template <typename T>
    [[nodiscard]] std::optional<std::vector<T>> readList(
        std::expected<std::vector<T>, ParseError> (*map)(std::string_view, OutputExtent),
        ProcessOutcome const& outcome)
    {
        auto const extent = readableExtent(judge(outcome));
        if (!extent || map == nullptr)
            return std::nullopt;
        auto mapped = map(outcome.stdoutText, *extent);
        if (!mapped)
            return std::nullopt;
        return std::move(*mapped);
    }

    /// @return @p list as a span, or std::nullopt when there is none.
    template <typename T>
    [[nodiscard]] std::optional<std::span<T const>> viewOf(std::optional<std::vector<T>> const& list) noexcept
    {
        if (!list)
            return std::nullopt;
        return std::span<T const> { *list };
    }

    /// The query a key lookup runs as, and its source.
    struct LookupQuery
    {
        IssueQuery const* query;      ///< The query: its site builds the tickets' pages.
        IssueSourceRow const* source; ///< Its source: the command and argument the lookup runs.
    };

    /// @param repo A configured repository.
    /// @param rows The issue sources.
    /// @return @p repo's first query whose source searches by JQL; std::nullopt: none.
    [[nodiscard]] std::optional<LookupQuery> lookupQueryOf(Repo const& repo,
                                                           std::span<IssueSourceRow const> rows) noexcept
    {
        for (auto const& query: repo.issueQueries)
            if (auto const* const source = findIssueSource(rows, query.source);
                source != nullptr && source->configKey == JqlKey && !source->command.empty()
                && source->map != nullptr)
                return LookupQuery { .query = &query, .source = source };
        return std::nullopt;
    }

    /// @return The Jira keys of @p issues, in order.
    [[nodiscard]] std::vector<std::string> jiraKeysOf(std::span<Issue const> issues)
    {
        auto keys = std::vector<std::string> {};
        for (auto const& issue: issues)
            if (auto const* const ref = std::get_if<JiraRef>(&issue.item))
                keys.push_back(ref->key);
        return keys;
    }

    /// Sets @p state's change requests: as listed, joined through @p provider's row with the pipelines and
    /// failed jobs when the row has a join.
    template <typename RepoState>
    void enrichChangeRequests(RepoState& state, Provider const& provider)
    {
        state.changeRequests = provider.enrich != nullptr ? provider.enrich(state.listedChangeRequests,
                                                                            viewOf(state.pipelines),
                                                                            viewOf(state.failedJobs))
                                                          : state.listedChangeRequests;
    }

    /// Rebuilds @p state's issues from every query's list, de-duplicated by URL, each with its progress (an
    /// issue several queries list is in progress if any of them says so), and those no change request
    /// claims.
    template <typename RepoState>
    void rebuildIssues(RepoState& state)
    {
        auto issues = std::vector<ListedIssue> {};
        for (auto const query: std::views::iota(std::size_t { 0 }, state.issuesByQuery.size()))
        {
            auto const keys = query < state.inProgressByQuery.size() ? viewOf(state.inProgressByQuery[query])
                                                                     : std::nullopt;
            for (auto const& found: state.issuesByQuery[query])
            {
                auto const progress = progressOf(found, keys);
                auto const known = std::ranges::find_if(
                    issues, [&](ListedIssue const& listed) { return listed.issue.url == found.url; });
                if (known == issues.end())
                    issues.push_back(ListedIssue { .issue = found, .progress = progress });
                else if (progress == Progress::InProgress)
                    known->progress = Progress::InProgress;
            }
        }
        state.issues = std::move(issues);
        state.unclaimed = unclaimed(
            std::ranges::to<std::vector<Issue>>(state.issues | std::views::transform(&ListedIssue::issue)),
            state.changeRequests);
    }

    [[nodiscard]] std::string line(std::string_view text)
    {
        return displayText(text, MaxSummaryLineChars);
    }

    /// Takes in a pipeline or failed-job list, and joins it into @p state's change requests. A failure leaves
    /// std::nullopt, which the join reads as a list that could not be read, and a line saying so.
    /// @param list PollList::Pipelines or PollList::FailedJobs.
    template <typename RepoState>
    void readCiList(PollList list,
                    Repo const& repo,
                    Provider const& provider,
                    ProcessOutcome const& outcome,
                    RepoState& state,
                    std::vector<std::string>& lines)
    {
        if (list == PollList::Pipelines)
        {
            state.pipelines = readList(provider.mapPipelines, outcome);
            if (!state.pipelines)
                lines.push_back(line(std::format("{}: the pipeline list could not be read; CI and review are "
                                                 "inferred from {}'s merge status",
                                                 repo.name,
                                                 provider.name)));
        }
        else
        {
            state.failedJobs = readList(provider.mapFailedJobs, outcome);
            if (!state.failedJobs)
                lines.push_back(line(std::format(
                    "{}: the failed job list could not be read; failing jobs are not named", repo.name)));
        }
        enrichChangeRequests(state, provider);
    }

    /// The lines a failed poll command leaves: why, and for a list, that it is incomplete.
    [[nodiscard]] std::vector<std::string> failureLines(std::string_view repo,
                                                        std::string_view repoPath,
                                                        std::string_view label,
                                                        Judgement const& judgement)
    {
        if (judgement.verdict == Verdict::CwdMissing)
            return { line(std::format("{}: {} does not exist", repo, repoPath)),
                     line(std::format("{}: {} is incomplete", repo, label)) };
        return { line(std::format("{}: {} failed: {}", repo, label, judgement.why)),
                 line(std::format("{}: {} is incomplete", repo, label)) };
    }

    /// @return Whether @p tag is a login check's: the forge's, or an issue source's (`poll/auth:<source>`).
    [[nodiscard]] bool isAuthTag(std::string_view tag) noexcept
    {
        return setOfSetupTag(tag, AuthTag).has_value();
    }

    /// @return The tag of @p source's login check.
    [[nodiscard]] std::string sourceAuthTag(IssueSourceRow const& source)
    {
        return setupTagOf(AuthTag, source.name);
    }

    /// @return Whether @p tag is a check of the setup rather than a repository's list: the hooks, the
    ///         plugins, a plugin's install step, or a login.
    [[nodiscard]] bool isSetupTag(std::string_view tag) noexcept
    {
        return isSetCheckTag(tag) || tag == PluginStepTag || isAuthTag(tag);
    }

    /// What a repository's lists show: on the dashboard, on the Work board, in the launcher and in what a
    /// dispatch would do.
    struct ShownLists
    {
        std::vector<ChangeRequest> changeRequests;
        std::vector<ListedIssue> issues;
        std::vector<Issue> unclaimed;
        std::optional<std::string> defaultBranch;
        std::vector<WorktreeEntry> worktrees;
        CheckoutUnknown unknownCheckout = CheckoutUnknown::Pending;
        [[nodiscard]] bool operator==(ShownLists const&) const = default;
    };

    /// @return What @p state shows, copied.
    template <typename RepoState>
    [[nodiscard]] ShownLists shownOf(RepoState const& state)
    {
        return ShownLists { .changeRequests = state.changeRequests,
                            .issues = state.issues,
                            .unclaimed = state.unclaimed,
                            .defaultBranch = state.defaultBranch,
                            .worktrees = state.worktrees,
                            .unknownCheckout = state.unknownCheckout };
    }

    /// @return The problem lines @p problems holds for @p tag; none when it holds none.
    [[nodiscard]] std::vector<std::string> problemsOf(
        std::map<std::string, std::vector<std::string>, std::less<>> const& problems, std::string_view tag)
    {
        auto const found = problems.find(tag);
        return found != problems.end() ? found->second : std::vector<std::string> {};
    }

    /// The line a failed login check leaves. Only whether it succeeded is shown, never what it printed: a
    /// login check's output may hold a token.
    /// @param who What it checks the login of, such as `GitHub` or `Jira`.
    /// @param repo The repository it ran in.
    /// @param command The declared login check.
    /// @param judgement Its outcome, judged.
    /// @param outcome Its outcome.
    [[nodiscard]] std::string authFailureLine(std::string_view who,
                                              Repo const& repo,
                                              std::string_view command,
                                              Judgement const& judgement,
                                              ProcessOutcome const& outcome)
    {
        if (judgement.verdict == Verdict::CwdMissing)
            return line(std::format("{}: {} does not exist", repo.name, repo.path));
        if (outcome.start == StartResult::Refused) // the host's words, not the check's output
            return line(std::format("{}: the login check could not start: {}", who, judgement.why));
        if (outcome.completion == Completion::TimedOut)
            return line(std::format("{}: the login check timed out", who));
        // The check fails for an expired token, a locked keyring or a network error as much as for no login:
        // the line says how to see which, by running the check itself.
        auto const* const check = findCommand(command);
        auto how = std::string {};
        for (auto const word: check != nullptr ? check->argv : std::span<std::string_view const> {})
            how += how.empty() ? std::string(word) : std::format(" {}", word);
        return line(std::format("{}: the login check failed (run {})", who, how));
    }

    /// The notices a list that was read may still need: that the forge or the host's limit left some out.
    template <typename T>
    [[nodiscard]] std::vector<std::string> listNotices(std::string_view repo,
                                                       std::string_view label,
                                                       Listed<T> const& listed)
    {
        auto lines = std::vector<std::string> {};
        switch (listed.completeness)
        {
            case ListCompleteness::Complete: break;
            case ListCompleteness::AtLimit:
                lines.push_back(line(
                    std::format("{}: {} shows the first {}; there may be more", repo, label, ListLimit)));
                break;
            case ListCompleteness::Cut:
                lines.push_back(
                    line(std::format("{}: {} is incomplete: its output was cut at the host's limit "
                                     "after {} entries",
                                     repo,
                                     label,
                                     listed.items.size() + listed.skipped)));
                break;
        }
        if (listed.skipped > 0)
            lines.push_back(line(
                std::format("{}: {} left out {} entries it could not read", repo, label, listed.skipped)));
        return lines;
    }

    /// What a failed read of a shown board makes of it.
    struct BoardFailure
    {
        FileReadError error = FileReadError::Unreadable; ///< Why the read failed.
        BoardState state = BoardState::Unavailable;      ///< What the board is then.
    };

    /// Every read failure that is not Unavailable; the rest are, with their reason kept.
    constexpr auto BoardFailures = std::array {
        BoardFailure { .error = FileReadError::NotRegular, .state = BoardState::Absent },
        BoardFailure { .error = FileReadError::DirectoryMissing, .state = BoardState::Gone },
    };

    /// @return The board's state after a read that failed with @p error.
    [[nodiscard]] BoardState boardStateOf(FileReadError error) noexcept
    {
        // A loop rather than std::ranges::find, for the reason Sanitize.cpp's dispositionOf() gives.
        for (auto const& row: BoardFailures)
            if (row.error == error)
                return row.state;
        return BoardState::Unavailable;
    }

    /// The run a shown board is of: what it is found by.
    constexpr auto BoardRunOf = [](auto const& shown) -> std::string const& {
        return shown.board.run;
    };
} // namespace

namespace engine::detail
{
    std::vector<std::string> claudeSetsOf(Config const& config)
    {
        auto sets = std::vector<std::string> {};
        for (auto const& worker: config.workers())
            if (agentRowOf(worker.agent).claudeState == ClaudeState::Read
                && !std::ranges::contains(sets, worker.envSet))
                sets.push_back(worker.envSet);
        return sets;
    }

    bool hasImplicitWorkerAlone(Config const& config) noexcept
    {
        auto const workers = config.workers();
        return workers.size() == 1 && workers.front().id == ImplicitWorkerId
               && workers.front().agent == AgentKind::Claude && workers.front().envSet.empty();
    }
} // namespace engine::detail

// {{{ repositories

Engine::RepoState* Engine::findRepoState(std::string_view name) noexcept
{
    auto const state = std::ranges::find(_repos, name, &RepoState::name);
    return state != _repos.end() ? &*state : nullptr;
}

Engine::RepoState const* Engine::findRepoState(std::string_view name) const noexcept
{
    auto const state = std::ranges::find(_repos, name, &RepoState::name);
    return state != _repos.end() ? &*state : nullptr;
}

uint32_t Engine::failingChangeRequests() const noexcept
{
    auto count = uint32_t { 0 };
    for (auto const& repo: _repos)
        count += static_cast<uint32_t>(
            std::ranges::count(repo.changeRequests, CiState::Failing, &ChangeRequest::ci));
    return count;
}

void Engine::refreshOrphans()
{
    for (auto& repo: _repos)
    {
        auto found = repo.worktrees.empty() || !_config ? std::vector<WorktreeEntry> {}
                                                        : orchestrator::orphans(_runs,
                                                                                _config->worktreeRoot(),
                                                                                repo.worktrees,
                                                                                repo.worktreesListedMonoMs,
                                                                                _config->pathStyle());
        // The dashboard lists them: a change is drawn.
        if (found != repo.orphans)
        {
            repo.orphans = std::move(found);
            ++_revision;
        }
    }
}

// }}}
// {{{ polls

Engine::Issued Engine::issuePoll(std::string tag,
                                 std::string_view command,
                                 std::vector<Argument> arguments,
                                 std::string cwd,
                                 std::vector<Effect>& out)
{
    auto const* const decl = findCommand(command);
    if (decl == nullptr)
    {
        // A provider row naming an undeclared command is a defect, shown rather than waited for.
        _pollProblems[tag] = { line(std::format("{} is not a declared command; nothing was run", command)) };
        ++_revision;
        return Issued::No;
    }
    issue(RunCommand { .tag = std::move(tag),
                       .command = decl->name,
                       .arguments = std::move(arguments),
                       .cwd = std::move(cwd) },
          Priority::Poll,
          out);
    return Issued::Yes;
}

void Engine::startPoll(std::vector<Effect>& out)
{
    if (!_config || _config->repos().empty())
        return;
    _pollResult = PollResult::Clean;
    _lookupPhase = LookupPhase::NotIssued;
    recheckFinished(out);
    auto const& first = _config->repos().front();
    auto const* const provider = findProvider(_config->sources().providers, first.forge);
    if (provider != nullptr && _auth == AuthState::LoggedIn && _authCheck == AuthCheck::Skip)
        issueLists(out);
    else if (provider != nullptr
             && issuePoll(std::string(AuthTag), provider->authCheck, {}, first.path, out) == Issued::Yes)
    {
        ++_pollOutstanding;
        issueSourceAuthChecks(out);
    }
    // Nothing handed to the host: the poll failed, and the next one is still scheduled, backed off.
    if (_pollOutstanding == 0)
    {
        _pollResult = PollResult::Failed;
        onListAnswered(out);
    }
    issueWorktreeLists(out);
}

void Engine::issueSourceAuthChecks(std::vector<Effect>& out)
{
    // Each issue source a repository lists from that has a login of its own is checked once, in the first
    // repository that names it; an Unmeasured one never runs.
    auto checked = std::vector<std::string_view> {};
    for (auto const& repo: _config->repos())
        for (auto const& query: repo.issueQueries)
        {
            auto const* const source = findIssueSource(_config->sources().issueSources, query.source);
            if (source == nullptr || source->authCheck.empty()
                || source->verification != Verification::Recorded
                || std::ranges::contains(checked, source->authCheck))
                continue;
            checked.push_back(source->authCheck);
            if (issuePoll(sourceAuthTag(*source), source->authCheck, {}, repo.path, out) == Issued::Yes)
                ++_pollOutstanding;
        }
}

bool Engine::isResumable(std::string_view tag) const noexcept
{
    if (isAuthTag(tag) || isSetCheckTag(tag))
        return true;
    // A list of a repository no longer configured is answered to nobody.
    auto const parsed = parsePollTag(tag);
    return parsed && findRepoState(parsed->repo) != nullptr;
}

bool Engine::isForgePoll(std::string_view tag) const noexcept
{
    if (isAuthTag(tag))
        return true;
    auto const parsed = parsePollTag(tag);
    return parsed && parsed->list != PollList::Worktrees && findRepoState(parsed->repo) != nullptr;
}

void Engine::startOrResumePoll(StartContext const& context, std::vector<Effect>& out)
{
    // A previous instance's poll commands are this one's: the ones the host still runs are awaited, and the
    // ones that finished are taken in below, rather than run again.
    // Kept for the whole start: a list whose result is taken in below is never run again, by this function
    // or by an answer it feeds (the login check's issues the lists).
    auto& answered = _answeredAtStart;
    answered.clear();
    for (auto const& result: context.finished)
        if (isResumable(result.tag))
            answered.push_back(result.tag);
    for (auto const& tag: context.inFlight)
    {
        if (!isResumable(tag))
            continue;
        // Its timeout class is not known here: the longer one.
        _inFlight.push_back(InFlightCall {
            .tag = tag,
            .deadlineMs =
                later(_monoMs, static_cast<uint64_t>((LongTimeout + CallDeadlineMargin).count())) });
        if (isForgePoll(tag))
            ++_pollOutstanding;
    }
    for (auto const& set: engine::detail::claudeSetsOf(*_config))
        if (!std::ranges::contains(answered, setupTagOf(HooksTag, set)))
            checkHooksOf(set, out);

    auto const forgeAnswered = static_cast<std::size_t>(
        std::ranges::count_if(answered, [&](std::string const& tag) { return isForgePoll(tag); }));
    if (_pollOutstanding == 0 && forgeAnswered == 0)
        startPoll(out);
    else
    {
        // The results are counted before any is taken in, so that the poll ends with the last of them.
        _pollOutstanding += forgeAnswered;
        _pollResult = PollResult::Clean;
        _lookupPhase = LookupPhase::NotIssued;
        recheckFinished(out);
        // A list is only ever run once the login was confirmed; the lists the previous instance had not run,
        // or whose results it took in before it went, are run now.
        auto const isList = [&](std::string_view tag) {
            return !isAuthTag(tag) && isForgePoll(tag);
        };
        if (std::ranges::any_of(answered, isList) || std::ranges::any_of(context.inFlight, isList))
        {
            _auth = AuthState::LoggedIn;
            _authCheck = AuthCheck::Skip;
            issueLists(out);
        }
        issueWorktreeLists(out);
    }
    for (auto const& result: context.finished)
        if (std::ranges::contains(answered, result.tag))
            onPollResult(result.tag, result.outcome, out);
}

void Engine::issueWorktreeLists(std::vector<Effect>& out)
{
    // The worktree lists need no forge: cleanup works while `gh` is logged out.
    for (auto const& repo: _config->repos())
        issueWorktreeList(repo, out);
}

void Engine::issueWorktreeList(Repo const& repo, std::vector<Effect>& out)
{
    auto tag = std::format("{}{}/{}", PollTagPrefix, repo.name, WorktreesList);
    if (isPending(tag) || std::ranges::contains(_answeredAtStart, tag))
        return;
    if (issuePoll(std::move(tag), WorktreeListCommand, {}, repo.path, out) == Issued::Yes)
        if (auto* const state = findRepoState(repo.name))
            state->worktreesAskedMonoMs = _monoMs;
}

void Engine::relistAfterFinishedRuns(std::vector<Effect>& out)
{
    if (!_config)
        return;
    for (auto const& repo: _config->repos())
    {
        auto const* const state = findRepoState(repo.name);
        // A listing that keeps failing is asked for again by the poll, not on every step.
        if (state == nullptr || state->worktreeListFailures != 0)
            continue;
        auto const isStale = [&](Run const& run) {
            return run.repo == repo.name && finishedSince(run, state->worktreesListedMonoMs);
        };
        if (std::ranges::any_of(_runs, isStale))
            issueWorktreeList(repo, out);
    }
}

void Engine::issueLists(std::vector<Effect>& out)
{
    for (auto const& repo: _config->repos())
    {
        auto const* const provider = findProvider(_config->sources().providers, repo.forge);
        auto const* const state = findRepoState(repo.name);
        if (provider == nullptr || state == nullptr)
            continue;
        // Only a command handed to the host is waited for; one it runs or answered already is not run again.
        auto const issueList =
            [&](std::string_view list, std::string_view command, std::vector<Argument> arguments) {
                auto tag = std::format("{}{}/{}", PollTagPrefix, repo.name, list);
                if (isPending(tag) || std::ranges::contains(_answeredAtStart, tag))
                    return;
                if (issuePoll(std::move(tag), command, std::move(arguments), repo.path, out) == Issued::Yes)
                    ++_pollOutstanding;
            };
        if (!repo.base && !state->defaultBranch)
            issueList(DefaultBranchList, provider->defaultBranch, {});
        issueList(PrsList, provider->listChangeRequests, {});
        // A forge whose change-request list carries no CI detail reads it from two more lists.
        if (!provider->listPipelines.empty())
            issueList(PipelinesList, provider->listPipelines, {});
        if (!provider->listFailedJobs.empty())
            issueList(FailedJobsList, provider->listFailedJobs, {});
        auto index = std::size_t { 0 };
        for (auto const& query: repo.issueQueries)
        {
            auto const queryIndex = index++;
            // The configuration accepted the source, so it has a row; the forge's lists through the provider.
            auto const* const source = findIssueSource(_config->sources().issueSources, query.source);
            if (source == nullptr)
                continue;
            auto const command = source->command.empty() ? provider->listIssues : source->command;
            issueList(std::format("{}{}", IssuesList, queryIndex),
                      command,
                      { Argument { .name = source->argument, .value = query.text } });
            // The same search, narrowed to what the user counts as in progress.
            if (!query.inProgressJql.empty())
                issueList(
                    std::format("{}{}", ProgressList, queryIndex),
                    command,
                    { Argument { .name = source->argument,
                                 .value = std::format("({}) AND ({})", query.text, query.inProgressJql) } });
        }
    }
}

HooksState Engine::hooksFor(std::string_view envSet) const noexcept
{
    auto const found = _hooks.find(envSet);
    return found != _hooks.end() ? found->second : HooksState::Unknown;
}

std::span<PluginRequirement const> Engine::missingPluginsFor(std::string_view envSet) const noexcept
{
    auto const found = _missingPluginsBySet.find(envSet);
    return found != _missingPluginsBySet.end() ? std::span<PluginRequirement const> { found->second }
                                               : std::span<PluginRequirement const> {};
}

HooksState Engine::hooks() const noexcept
{
    if (!_config)
        return HooksState::Unknown;
    auto const readsClaude = [](WorkerDef const& worker) {
        return agentRowOf(worker.agent).claudeState == ClaudeState::Read;
    };
    // No worker reads Claude Code's state: nothing was checked, so nothing is said to be there.
    if (std::ranges::none_of(_config->workers(), readsClaude))
        return HooksState::Unknown;
    auto worst = HooksState::Present;
    for (auto const& worker: _config->workers())
    {
        if (!readsClaude(worker))
            continue;
        switch (hooksFor(worker.envSet))
        {
            case HooksState::Missing: return HooksState::Missing;
            case HooksState::Unknown: worst = HooksState::Unknown; break;
            case HooksState::Present: break;
        }
    }
    return worst;
}

std::vector<std::string> Engine::namedWorkersOn(std::string_view envSet) const
{
    auto named = std::vector<std::string> {};
    if (!_config || engine::detail::hasImplicitWorkerAlone(*_config))
        return named;
    for (auto const& worker: _config->workers())
        if (agentRowOf(worker.agent).claudeState == ClaudeState::Read && worker.envSet == envSet)
            named.push_back(worker.id);
    return named;
}

void Engine::checkHooks(std::vector<Effect>& out)
{
    for (auto const& set: engine::detail::claudeSetsOf(*_config))
        checkHooksOf(set, out);
}

void Engine::checkHooksOf(std::string_view envSet, std::vector<Effect>& out)
{
    auto tag = setupTagOf(HooksTag, envSet);
    if (isPending(tag))
        return;
    // In the worktree root, which the host creates if it is missing. Every set's check is issued at once: a
    // poll waits behind the runs' commands for a slot.
    static_cast<void>(
        issuePoll(std::move(tag), HooksCommand, withSet({}, envSet), _config->worktreeRoot(), out));
}

void Engine::checkPlugins(std::vector<Effect>& out)
{
    for (auto const& set: engine::detail::claudeSetsOf(*_config))
        checkPluginsOf(set, out);
}

void Engine::checkPluginsOf(std::string_view envSet, std::vector<Effect>& out)
{
    auto tag = setupTagOf(PluginsTag, envSet);
    if (_config->plugins().empty())
    {
        if (auto const found = _missingPluginsBySet.find(envSet); found != _missingPluginsBySet.end())
            _missingPluginsBySet.erase(found);
        collectMissingPlugins();
        _pollProblems.erase(tag);
        return;
    }
    if (isPending(tag))
        return;
    // In the worktree root, as the hooks check: it always exists, and `claude` reads no project there.
    static_cast<void>(
        issuePoll(std::move(tag), PluginsCommand, withSet({}, envSet), _config->worktreeRoot(), out));
}

void Engine::onPluginsResult(std::string_view envSet, ProcessOutcome const& outcome)
{
    auto const judgement = judge(outcome);
    auto const whose = forWorkers(namedWorkersOn(envSet));
    auto& lines = _pollProblems[setupTagOf(PluginsTag, envSet)];
    lines.clear();
    auto& missing = _missingPluginsBySet[std::string(envSet)];
    missing.clear();
    if (judgement.verdict != Verdict::Ok)
        // A `claude` that is missing is said by the agents' own failure; this says which check it was.
        lines.push_back(
            line(std::format("the Claude Code plugins check{} failed: {}", whose, judgement.why)));
    else if (auto const enabled = enabledPluginsOf(outcome.stdoutText))
        missing = orchestrator::missingPlugins(_config->plugins(), *enabled);
    else
        lines.push_back(
            line(std::format("the Claude Code plugins check{} printed something unreadable", whose)));
    collectMissingPlugins();
}

void Engine::collectMissingPlugins()
{
    _missingPlugins.clear();
    if (!_config)
        return;
    for (auto const& plugin: _config->plugins())
    {
        auto const lacks = [&](std::vector<PluginRequirement> const& missing) {
            return std::ranges::contains(missing, plugin.id, &PluginRequirement::id);
        };
        if (std::ranges::any_of(_missingPluginsBySet | std::views::values, lacks))
            _missingPlugins.push_back(plugin);
    }
}

void Engine::installPlugins(std::vector<Effect>& out)
{
    if (!_config || _missingPlugins.empty() || !_pluginSteps.empty())
        return;
    // One queue for every set, in the order of the workers: each set's Claude Code installs what it lacks.
    // A marketplace already known makes `marketplace add` fail; the install that follows still runs, and it
    // is what is judged.
    for (auto const& set: engine::detail::claudeSetsOf(*_config))
    {
        auto const missing = _missingPluginsBySet.find(set);
        if (missing == _missingPluginsBySet.end())
            continue;
        for (auto const& plugin: missing->second)
        {
            if (!plugin.marketplace.empty())
                _pluginSteps.push_back(PluginStep {
                    .command = std::string(MarketplaceAddCommand),
                    .args = withSet({ Argument { .name = "source", .value = plugin.marketplace } }, set),
                    .plugin = plugin.id,
                    .envSet = set });
            _pluginSteps.push_back(
                PluginStep { .command = std::string(PluginInstallCommand),
                             .args = withSet({ Argument { .name = "plugin", .value = plugin.id } }, set),
                             .plugin = plugin.id,
                             .envSet = set });
        }
    }
    _pollProblems.erase(std::string(PluginStepTag));
    ++_revision;
    issuePluginStep(out);
}

void Engine::issuePluginStep(std::vector<Effect>& out)
{
    if (_pluginSteps.empty())
    {
        // Every step ran: what is installed now is checked again.
        checkPlugins(out);
        return;
    }
    auto const& step = _pluginSteps.front();
    if (issuePoll(std::string(PluginStepTag), step.command, step.args, _config->worktreeRoot(), out)
        == Issued::No)
        _pluginSteps.clear();
}

void Engine::onPluginStepResult(ProcessOutcome const& outcome, std::vector<Effect>& out)
{
    if (_pluginSteps.empty())
        return;
    auto const step = std::move(_pluginSteps.front());
    _pluginSteps.pop_front();
    auto const judgement = judge(outcome);
    if (step.command == PluginInstallCommand && judgement.verdict != Verdict::Ok)
        _pollProblems[std::string(PluginStepTag)].push_back(
            line(std::format("installing {}{} failed: {}; run by hand: claude plugin install {}",
                             step.plugin,
                             forWorkers(namedWorkersOn(step.envSet)),
                             judgement.why,
                             step.plugin)));
    if (!_config)
    {
        _pluginSteps.clear();
        return;
    }
    issuePluginStep(out);
}

void Engine::onHooksResult(std::string_view envSet, ProcessOutcome const& outcome)
{
    auto const judgement = judge(outcome);
    auto const ok = judgement.verdict == Verdict::Ok;
    auto const state = ok ? hooksStateOf(outcome.stdoutText) : HooksState::Unknown;
    _hooks.insert_or_assign(std::string(envSet), state);
    auto const whose = forWorkers(namedWorkersOn(envSet));
    auto& lines = _pollProblems[setupTagOf(HooksTag, envSet)];
    lines.clear();
    // Every set's check runs in the worktree root: that it is missing is said once, under the check's own
    // tag (the implicit worker's), and unsaid by any set's check that ran there.
    auto const rootMissing = line(std::format("{} does not exist", _config->worktreeRoot()));
    auto& rootLines = _pollProblems[std::string(HooksTag)];
    std::erase(rootLines, rootMissing);
    if (judgement.verdict == Verdict::CwdMissing)
        rootLines.push_back(rootMissing);
    else if (!ok)
        lines.push_back(line(std::format("the agent hooks check{} failed: {}", whose, judgement.why)));
    else if (state == HooksState::Unknown)
        lines.push_back(line(std::format("the agent hooks check{} printed something unreadable", whose)));
}

void Engine::onAuthResult(ProcessOutcome const& outcome, std::vector<Effect>& out)
{
    auto const judgement = judge(outcome);
    --_pollOutstanding;
    auto& lines = _pollProblems[std::string(AuthTag)];
    lines.clear();
    // Its exit code answers: what it printed does not matter.
    if (judgement.verdict == Verdict::Ok || judgement.verdict == Verdict::Cut)
    {
        _auth = AuthState::LoggedIn;
        _authCheck = AuthCheck::Skip;
        issueLists(out);
    }
    else
    {
        _auth = AuthState::LoggedOut;
        _pollResult = PollResult::Failed;
        auto const& first = _config->repos().front();
        // The configuration accepted the forge, so it has a row.
        auto const* const provider = findProvider(_config->sources().providers, first.forge);
        lines.push_back(authFailureLine(provider != nullptr ? provider->name : forgeName(first.forge),
                                        first,
                                        provider != nullptr ? provider->authCheck : std::string_view {},
                                        judgement,
                                        outcome));
    }
    onListAnswered(out);
}

void Engine::onSourceAuthResult(std::string_view tag, ProcessOutcome const& outcome, std::vector<Effect>& out)
{
    auto const judgement = judge(outcome);
    --_pollOutstanding;
    auto& lines = _pollProblems[std::string(tag)];
    lines.clear();
    // Its exit code answers, as the forge's does: what it printed is released unread.
    auto const sources = _config->sources().issueSources;
    auto const source =
        std::ranges::find_if(sources, [&](IssueSourceRow const& row) { return sourceAuthTag(row) == tag; });
    auto const repo = std::ranges::find_if(_config->repos(), [&](Repo const& configured) {
        return std::ranges::contains(
            configured.issueQueries, source != sources.end() ? source->name : "", &IssueQuery::source);
    });
    if (judgement.verdict != Verdict::Ok && judgement.verdict != Verdict::Cut && source != sources.end()
        && repo != _config->repos().end())
    {
        _pollResult = PollResult::Failed;
        lines.push_back(authFailureLine(source->title, *repo, source->authCheck, judgement, outcome));
    }
    onListAnswered(out);
}

void Engine::onPollResult(std::string_view tag, ProcessOutcome const& outcome, std::vector<Effect>& out)
{
    // A repository's list that came back as it was changes nothing drawn: a poll every two minutes over
    // several repositories and queries costs a redraw only where a list, or a problem it reports, changed.
    if (isSetupTag(tag))
    {
        ++_revision;
        onSetupResult(tag, outcome, out);
        return;
    }
    auto const parsed = parsePollTag(tag);
    auto const* const state = parsed ? findRepoState(parsed->repo) : nullptr;
    if (state == nullptr)
        return;
    auto const shownBefore = shownOf(*state);
    auto const problemsBefore = problemsOf(_pollProblems, tag);
    onListResult(tag, outcome, out);
    if (shownOf(*state) != shownBefore || problemsOf(_pollProblems, tag) != problemsBefore)
        ++_revision;
}

void Engine::onSetupResult(std::string_view tag, ProcessOutcome const& outcome, std::vector<Effect>& out)
{
    // A set's check answered after the configuration stopped naming the set (a previous instance's) is
    // answered to nobody.
    auto const isClaudeSet = [&](std::string_view set) {
        return std::ranges::contains(engine::detail::claudeSetsOf(*_config), set);
    };
    if (auto const set = setOfSetupTag(tag, HooksTag))
    {
        if (!isClaudeSet(*set))
            return;
        onHooksResult(*set, outcome);
        // A set's plugins are checked once its hooks are: one setup check per set at a time, so that neither
        // takes a command slot from the lists or a run.
        checkPluginsOf(*set, out);
        return;
    }
    if (auto const set = setOfSetupTag(tag, PluginsTag))
    {
        if (isClaudeSet(*set))
            onPluginsResult(*set, outcome);
        return;
    }
    if (tag == PluginStepTag)
    {
        onPluginStepResult(outcome, out);
        return;
    }
    if (tag == AuthTag)
    {
        onAuthResult(outcome, out);
        return;
    }
    if (isAuthTag(tag))
        onSourceAuthResult(tag, outcome, out);
}

void Engine::onListResult(std::string_view tag, ProcessOutcome const& outcome, std::vector<Effect>& out)
{
    auto const judgement = judge(outcome);
    auto const ok = judgement.verdict == Verdict::Ok;

    auto const parsed = parsePollTag(tag);
    auto* const state = parsed ? findRepoState(parsed->repo) : nullptr;
    auto const* const repo = parsed ? _config->findRepo(parsed->repo) : nullptr;
    auto const* const provider =
        repo != nullptr ? findProvider(_config->sources().providers, repo->forge) : nullptr;
    if (state == nullptr || provider == nullptr)
        return;
    auto& lines = _pollProblems[std::string(tag)];
    lines.clear();

    if (parsed->list == PollList::Worktrees)
    {
        // Not part of the poll's outcome: a failing git does not slow the forge down.
        if (ok)
        {
            state->worktrees = worktreeEntries(outcome.stdoutText);
            state->worktreesListedMonoMs = state->worktreesAskedMonoMs;
            state->worktreeListFailures = 0;
        }
        else
        {
            lines = failureLines(repo->name, repo->path, "the worktree list"sv, judgement);
            ++state->worktreeListFailures;
        }
        // An answer that names no main checkout, a directory that is gone, or a listing that keeps failing
        // is definitive: a run waiting for the main checkout stops waiting. Any other failure is not, yet.
        if (ok || judgement.verdict == Verdict::CwdMissing
            || state->worktreeListFailures >= MaxWorktreeListFailures)
            state->unknownCheckout = CheckoutUnknown::Unavailable;
        resumeCheckoutWaits(out);
        return;
    }

    --_pollOutstanding;
    // The lists the Work board reads besides fall back when they fail: their own line says so, and they
    // neither back the poll off nor ask for the login again.
    auto const answered = [&] {
        rebuildIssues(*state);
        onListAnswered(out);
    };
    switch (parsed->list)
    {
        case PollList::Pipelines:
        case PollList::FailedJobs:
            readCiList(parsed->list, *repo, *provider, outcome, *state, lines);
            answered();
            return;
        case PollList::Progress:
            onProgressResult(parsed->query, *repo, outcome, lines);
            answered();
            return;
        case PollList::Linked:
            onLinkedResult(*repo, outcome, lines);
            answered();
            return;
        case PollList::ChangeRequests:
        case PollList::Issues:
        case PollList::DefaultBranch:
        case PollList::Worktrees: break;
    }
    auto const query = parsed->list == PollList::Issues ? std::optional { parsed->query } : std::nullopt;
    auto const label = [&] {
        if (parsed->list == PollList::ChangeRequests)
            return std::string("the pull request list");
        if (parsed->list == PollList::DefaultBranch)
            return std::string("the default branch");
        if (query && *query < repo->issueQueries.size())
        {
            // A source other than the forge is named, so that a line about its tool says whose it is.
            auto const& issueQuery = repo->issueQueries[*query];
            auto const* const source = findIssueSource(_config->sources().issueSources, issueQuery.source);
            if (source != nullptr && source->map != nullptr)
                return std::format("the {} issue list \"{}\"", source->title, issueQuery.text);
            return std::format("the issue list \"{}\"", issueQuery.text);
        }
        return std::format("the list {}", tag);
    }();
    auto const failed = [&](Judgement const& why) {
        lines = failureLines(repo->name, repo->path, label, why);
        _pollResult = PollResult::Failed;
        _authCheck = AuthCheck::Needed;
    };
    // The host cuts the same output at the same place every time, so a cut is no failure: backing off or
    // asking for the login again would not change it. What was read before the cut is shown, marked
    // incomplete; if nothing was, the previous list stays.
    auto const cut = judgement.verdict == Verdict::Cut;
    auto const extent = cut ? OutputExtent::Truncated : OutputExtent::Whole;
    auto const unread = [&] {
        if (cut)
            lines = { line(std::format(
                "{}: {} is incomplete: its output was cut at the host's limit before a whole entry",
                repo->name,
                label)) };
        else
            failed(Judgement { .verdict = Verdict::Failed, .why = "its output could not be read" });
    };
    if (!ok && !cut)
        failed(judgement);
    else if (parsed->list == PollList::ChangeRequests)
    {
        if (auto listed = provider->changeRequests(outcome.stdoutText, extent))
        {
            lines = listNotices(repo->name, label, *listed);
            state->listedChangeRequests = std::move(listed->items);
            enrichChangeRequests(*state, *provider);
        }
        else
            unread();
    }
    else if (parsed->list == PollList::DefaultBranch)
    {
        if (auto branch = provider->defaultBranchOf(outcome.stdoutText))
            state->defaultBranch = std::move(*branch);
        else
            unread();
    }
    else if (query && *query < state->issuesByQuery.size() && *query < repo->issueQueries.size())
    {
        auto const& issueQuery = repo->issueQueries[*query];
        auto const* const source = findIssueSource(_config->sources().issueSources, issueQuery.source);
        auto listed =
            source != nullptr && source->map != nullptr
                ? source->map(outcome.stdoutText, extent, IssueSourceContext { .site = issueQuery.site })
                : provider->issues(outcome.stdoutText, extent);
        if (listed)
        {
            lines = listNotices(repo->name, label, *listed);
            state->issuesByQuery[*query] = std::move(listed->items);
        }
        else
            unread();
    }
    answered();
}

void Engine::onProgressResult(std::size_t query,
                              Repo const& repo,
                              ProcessOutcome const& outcome,
                              std::vector<std::string>& lines)
{
    auto* const state = findRepoState(repo.name);
    if (state == nullptr || query >= repo.issueQueries.size() || query >= state->inProgressByQuery.size())
        return;
    auto const& issueQuery = repo.issueQueries[query];
    auto const* const source = findIssueSource(_config->sources().issueSources, issueQuery.source);
    auto& keys = state->inProgressByQuery[query];
    keys = std::nullopt;
    auto const extent = readableExtent(judge(outcome));
    if (extent && source != nullptr && source->map != nullptr)
        if (auto const listed =
                source->map(outcome.stdoutText, *extent, IssueSourceContext { .site = issueQuery.site }))
            keys = jiraKeysOf(listed->items);
    // Without the keys, a ticket's own status category says whether it is in progress.
    if (!keys)
    {
        auto const who = source != nullptr ? source->title : std::string_view { issueQuery.source };
        lines.push_back(
            line(std::format("{}: the {} in-progress query \"{}\" failed; tickets in progress are "
                             "those {} counts as in progress",
                             repo.name,
                             who,
                             issueQuery.inProgressJql,
                             who)));
    }
}

void Engine::onLinkedResult(Repo const& repo, ProcessOutcome const& outcome, std::vector<std::string>& lines)
{
    // A lookup a previous instance issued counts as this poll's.
    _lookupPhase = LookupPhase::Issued;
    auto const lookup = lookupQueryOf(repo, _config->sources().issueSources);
    auto const extent = readableExtent(judge(outcome));
    auto tickets = std::optional<std::vector<Issue>> {};
    if (lookup && extent)
        if (auto listed = lookup->source->map(
                outcome.stdoutText, *extent, IssueSourceContext { .site = lookup->query->site }))
            tickets = std::move(listed->items);
    if (!tickets)
    {
        // The previous tickets stay: a failure is no empty list.
        lines.push_back(line(std::format("{}: the {} lookup of the tickets merge requests name failed; those "
                                         "merge requests are shown without their ticket",
                                         repo.name,
                                         lookup ? lookup->source->title : "ticket"sv)));
        return;
    }
    if (*tickets != _linkedTickets)
    {
        _linkedTickets = std::move(*tickets);
        ++_revision;
    }
}

Engine::Issued Engine::issueKeyLookup(std::vector<Effect>& out)
{
    _lookupPhase = LookupPhase::Issued;
    // Only after the lists were read: a poll that never got past the login has no new names.
    if (!_config || _auth != AuthState::LoggedIn)
        return Issued::No;
    auto const work = repoWork();
    auto const projects = jiraProjectsOf(_config->repos(), work);
    auto const isListed = [&](std::string_view key) {
        return std::ranges::any_of(work, [&](RepoWork const& repo) {
            return std::ranges::any_of(repo.issues, [&](ListedIssue const& listed) {
                auto const* const ref = std::get_if<JiraRef>(&listed.issue.item);
                return ref != nullptr && ref->key == key;
            });
        });
    };
    auto keys = std::vector<std::string> {};
    for (auto const& repo: work)
        for (auto const& mr: repo.changeRequests)
            if (auto key = linkedJiraKey(mr.title, mr.head, projects); key && !isListed(*key))
                keys.push_back(std::move(*key));
    std::ranges::sort(keys);
    auto const [first, last] = std::ranges::unique(keys);
    keys.erase(first, last);
    if (keys.size() > ListLimit)
        keys.resize(ListLimit);
    // Nothing to look up: no merge request names a ticket the lists left out, and what a failed lookup said
    // no longer holds.
    if (keys.empty())
    {
        for (auto const& repo: _config->repos())
            _pollProblems.erase(std::format("{}{}/{}", PollTagPrefix, repo.name, LinkedList));
        if (!_linkedTickets.empty())
        {
            _linkedTickets.clear();
            ++_revision;
        }
        return Issued::No;
    }
    auto const& repos = _config->repos();
    auto const sources = _config->sources().issueSources;
    auto const repo = std::ranges::find_if(
        repos, [&](Repo const& configured) { return lookupQueryOf(configured, sources).has_value(); });
    if (repo == repos.end())
        return Issued::No;
    auto const lookup = lookupQueryOf(*repo, sources);
    auto jql = std::string { "key in (" };
    for (auto const& key: keys)
        jql += std::format("{}{}", &key == &keys.front() ? "" : ", ", key);
    jql += ")";
    auto tag = std::format("{}{}/{}", PollTagPrefix, repo->name, LinkedList);
    if (isPending(tag)
        || issuePoll(std::move(tag),
                     lookup->source->command,
                     { Argument { .name = lookup->source->argument, .value = std::move(jql) } },
                     repo->path,
                     out)
               == Issued::No)
        return Issued::No;
    ++_pollOutstanding;
    return Issued::Yes;
}

void Engine::onListAnswered(std::vector<Effect>& out)
{
    if (_pollOutstanding > 0)
        return;
    // The other lists answered: the poll ends once the key lookup they lead to did.
    if (_lookupPhase == LookupPhase::NotIssued && issueKeyLookup(out) == Issued::Yes)
        return;
    if (_pollResult == PollResult::Failed)
        ++_pollFailures;
    else
        _pollFailures = 0;
    _nextPollMs = engine::detail::later(
        _monoMs,
        engine::detail::pollInterval(static_cast<uint64_t>(_config->pollEvery().count()), _pollFailures));
    drainQueue(out);
}

void Engine::recheckFinished(std::vector<Effect>& out)
{
    auto purge = std::vector<std::string> {};
    auto retry = std::vector<std::string> {};
    for (auto const& run: std::span(_runs).first(_insideCount))
    {
        auto const* const finished = std::get_if<Finished>(&run.state);
        if (finished == nullptr)
            continue;
        if (isPurgeable(run, _wallMs, _config->finishedRetention()))
            purge.push_back(run.id);
        // Until its main checkout is known (the first worktree listing), a check could only replace the
        // kept run's reason with "not known": it waits for a poll that knows it. One outside the
        // configuration, which the user ended, is never checked again: its repository is not configured.
        else if (finished->fate == Fate::Kept && !unknownCheckout(run) && scopeOf(run) != RunScope::Outside)
            retry.push_back(run.id);
    }
    for (auto const& id: purge)
    {
        out.emplace_back(StoreRemove { .key = runKey(id) });
        forget(id);
        ++_revision;
    }
    // A kept worktree cleaned up by hand is removed on the next poll.
    for (auto const& id: retry)
        feed(id, RetryRemoval {}, out);
}

void Engine::resumeCheckoutWaits(std::vector<Effect>& out)
{
    auto ids = std::vector<std::string> {};
    for (auto const& run: std::span(_runs).first(_insideCount))
        if (auto const* const ending = std::get_if<Ending>(&run.state);
            ending != nullptr && ending->step == EndStep::MainCheckout)
            ids.push_back(run.id);
    // A run whose main checkout is still not known stays as it is.
    for (auto const& id: ids)
        feed(id, CheckoutListed {}, out);
}

// }}}
// {{{ status boards

std::optional<uint64_t> Engine::boardDueAt(Run const& run) const
{
    // A blocked or done agent makes no tool calls: it writes no board. An agent that reports no status is
    // asked for none, as if `status_board` were off.
    auto const after = static_cast<uint64_t>(_config->statusBoardAfter().count());
    if (after == 0 || agentRowOf(run.agent).status == StatusReporting::Silent
        || !std::holds_alternative<Running>(run.state) || !run.pane || _boardsFound.contains(run.id))
        return std::nullopt;
    return later(run.runningSinceMs, after);
}

void Engine::readBoards(std::vector<Effect>& out)
{
    for (auto const& run: std::span(_runs).first(_insideCount))
        if (auto const due = boardDueAt(run); due && _monoMs >= *due)
            out.emplace_back(ReadFile { .tag = std::format("{}{}", run.id, BoardTagSuffix),
                                        .path = inWorktree(run, "STATUS.md") });
}

void Engine::onBoardRead(std::string_view runId, FileRead const& read)
{
    if (!read.content || findRun(runId) == nullptr)
        return;
    if (_boardsFound.emplace(runId).second)
        ++_revision;
}

Board const* Engine::board(std::string_view run) const noexcept
{
    auto const shown = std::ranges::find(_shownBoards, run, BoardRunOf);
    return shown != _shownBoards.end() ? &shown->board : nullptr;
}

bool Engine::hasBoard(std::string_view run) const noexcept
{
    return _boardsFound.contains(run);
}

void Engine::showBoard(ShowBoard const& request, std::vector<Effect>& out)
{
    auto const* const run = findRun(request.run);
    if (!_config || run == nullptr)
        return;
    auto shown = std::ranges::find(_shownBoards, request.run, BoardRunOf);
    // Shown again by the tab that shows it, the board is read now: the user asked for it.
    auto readNow = shown != _shownBoards.end() && std::ranges::contains(shown->tabs, request.tab);
    if (!readNow)
    {
        // A tab shows one board: the one it showed before is no longer shown there.
        hideBoard(request.tab);
        shown = std::ranges::find(_shownBoards, request.run, BoardRunOf);
        if (shown != _shownBoards.end())
            shown->tabs.push_back(request.tab);
        else
        {
            _shownBoards.push_back(ShownBoard {
                .board = Board { .run = request.run }, .tabs = { request.tab }, .nextReadMs = 0 });
            ++_revision;
            readNow = true;
        }
    }
    if (std::holds_alternative<Finished>(run->state))
        feed(request.run, MarkSeen {}, out);
    if (auto const* const current = findRun(request.run); readNow && current != nullptr)
        readShownBoard(*current, out);
}

void Engine::hideBoard(std::string_view tab)
{
    for (auto& shown: _shownBoards)
        std::erase(shown.tabs, tab);
    if (std::erase_if(_shownBoards, [](ShownBoard const& shown) { return shown.tabs.empty(); }) > 0)
        ++_revision;
}

void Engine::readShownBoard(Run const& run, std::vector<Effect>& out)
{
    auto const shown = std::ranges::find(_shownBoards, run.id, BoardRunOf);
    if (shown == _shownBoards.end())
        return;
    shown->nextReadMs = later(_monoMs, static_cast<uint64_t>(BoardRefreshEvery.count()));
    out.emplace_back(ReadFile { .tag = std::format("{}{}", run.id, BoardViewTagSuffix),
                                .path = inWorktree(run, "STATUS.md") });
}

void Engine::refreshShownBoards(std::vector<Effect>& out)
{
    auto due = std::vector<std::string> {};
    for (auto const& shown: _shownBoards)
        if (_monoMs >= shown.nextReadMs)
            due.push_back(shown.board.run);
    // A finished run's agent writes no more: its board is not read again, only when shown anew.
    for (auto const& id: due)
        if (auto const* const run = findRun(id); run != nullptr && isUnfinished(*run))
            readShownBoard(*run, out);
}

void Engine::onShownBoardRead(std::string_view runId, FileRead const& read)
{
    if (findRun(runId) == nullptr)
        return;
    if (read.content && _boardsFound.emplace(runId).second)
        ++_revision;
    auto const shown = std::ranges::find(_shownBoards, runId, BoardRunOf);
    if (shown == _shownBoards.end())
        return;
    auto next = Board { .run = std::string { runId } };
    if (read.content)
    {
        next.state = BoardState::Read;
        next.bytes = *read.content;
        next.modifiedWallMs = read.modifiedWallMs;
    }
    else
    {
        next.state = boardStateOf(read.content.error());
        if (next.state == BoardState::Unavailable)
            next.error = read.content.error();
    }
    // When it was read moves on with every read; only what the dashboard draws from it is a change.
    next.readMonoMs = read.monoMs != 0 ? read.monoMs : _monoMs;
    next.readWallMs = read.wallMs != 0 ? read.wallMs : _wallMs;
    auto const changed = next.state != shown->board.state || next.bytes != shown->board.bytes
                         || next.modifiedWallMs != shown->board.modifiedWallMs
                         || next.error != shown->board.error;
    shown->board = std::move(next);
    if (changed)
        ++_revision;
}

// }}}

} // namespace orchestrator
