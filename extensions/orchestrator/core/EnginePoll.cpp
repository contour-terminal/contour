// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/EngineDetail.hpp>
#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Provider.hpp>
#include <orchestrator/core/Sanitize.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    using engine::detail::BoardTagSuffix;
    using engine::detail::BoardViewTagSuffix;
    using engine::detail::judge;
    using engine::detail::Judgement;
    using engine::detail::later;
    using engine::detail::PollTagPrefix;
    using engine::detail::Verdict;

    constexpr auto AuthTag = "poll/auth"sv;
    constexpr auto HooksTag = "poll/hooks"sv;
    constexpr auto HooksCommand = "agent-setup-check"sv;
    constexpr auto WorktreeListCommand = "git-worktree-list"sv;

    /// What a repository's poll tag names after `poll/<repo>/`.
    constexpr auto PrsList = "prs"sv;
    constexpr auto IssuesList = "issues/"sv;
    constexpr auto DefaultBranchList = "default-branch"sv;
    constexpr auto WorktreesList = "worktrees"sv;

    /// A repository's poll tag, taken apart.
    struct PollTag
    {
        std::string_view repo; ///< The repository's name.
        std::string_view list; ///< What was listed, such as `prs` or `issues/0`.
    };

    [[nodiscard]] std::optional<PollTag> parsePollTag(std::string_view tag) noexcept
    {
        if (!tag.starts_with(PollTagPrefix))
            return std::nullopt;
        auto const rest = tag.substr(PollTagPrefix.size());
        auto const slash = rest.find('/');
        if (slash == std::string_view::npos)
            return std::nullopt;
        return PollTag { .repo = rest.substr(0, slash), .list = rest.substr(slash + 1) };
    }

    [[nodiscard]] std::optional<std::size_t> issueQueryIndex(std::string_view list) noexcept
    {
        if (!list.starts_with(IssuesList))
            return std::nullopt;
        auto const digits = list.substr(IssuesList.size());
        auto index = std::size_t { 0 };
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
        if (error != std::errc {} || end != digits.data() + digits.size())
            return std::nullopt;
        return index;
    }

    [[nodiscard]] std::string line(std::string_view text)
    {
        return displayText(text, MaxSummaryLineChars);
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

    /// @return Whether @p tag is a login check's: the forge's, or an issue source's (`poll/auth/<source>`).
    [[nodiscard]] bool isAuthTag(std::string_view tag) noexcept
    {
        return tag == AuthTag || (tag.starts_with(AuthTag) && tag.substr(AuthTag.size()).starts_with('/'));
    }

    /// @return The tag of @p source's login check.
    [[nodiscard]] std::string sourceAuthTag(IssueSourceRow const& source)
    {
        return std::format("{}/{}", AuthTag, source.name);
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
        auto found = repo.worktrees.empty() || !_config
                         ? std::vector<WorktreeEntry> {}
                         : orchestrator::orphans(_runs, _config->worktreeRoot(), repo.worktrees);
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
    if (isAuthTag(tag) || tag == HooksTag)
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
    return parsed && parsed->list != WorktreesList && findRepoState(parsed->repo) != nullptr;
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
    if (!std::ranges::contains(answered, HooksTag))
        checkHooks(out);

    auto const forgeAnswered = static_cast<std::size_t>(
        std::ranges::count_if(answered, [&](std::string const& tag) { return isForgePoll(tag); }));
    if (_pollOutstanding == 0 && forgeAnswered == 0)
        startPoll(out);
    else
    {
        // The results are counted before any is taken in, so that the poll ends with the last of them.
        _pollOutstanding += forgeAnswered;
        _pollResult = PollResult::Clean;
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
    {
        auto tag = std::format("{}{}/{}", PollTagPrefix, repo.name, WorktreesList);
        if (!isPending(tag) && !std::ranges::contains(_answeredAtStart, tag))
            static_cast<void>(issuePoll(std::move(tag), WorktreeListCommand, {}, repo.path, out));
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
        auto index = std::size_t { 0 };
        for (auto const& query: repo.issueQueries)
        {
            auto const list = std::format("{}{}", IssuesList, index++);
            // The configuration accepted the source, so it has a row; the forge's lists through the provider.
            if (auto const* const source = findIssueSource(_config->sources().issueSources, query.source))
                issueList(list,
                          source->command.empty() ? provider->listIssues : source->command,
                          { Argument { .name = source->argument, .value = query.text } });
        }
    }
}

void Engine::checkHooks(std::vector<Effect>& out)
{
    if (isPending(HooksTag))
        return;
    // In the worktree root, which the host creates if it is missing.
    static_cast<void>(issuePoll(std::string(HooksTag), HooksCommand, {}, _config->worktreeRoot(), out));
}

void Engine::onHooksResult(ProcessOutcome const& outcome)
{
    auto const judgement = judge(outcome);
    auto const ok = judgement.verdict == Verdict::Ok;
    _hooks = ok ? hooksStateOf(outcome.stdoutText) : HooksState::Unknown;
    auto& lines = _pollProblems[std::string(HooksTag)];
    lines.clear();
    if (judgement.verdict == Verdict::CwdMissing)
        lines.push_back(line(std::format("{} does not exist", _config->worktreeRoot())));
    else if (!ok)
        lines.push_back(line(std::format("the agent hooks check failed: {}", judgement.why)));
    else if (_hooks == HooksState::Unknown)
        lines.emplace_back("the agent hooks check printed something unreadable");
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
    ++_revision;
    if (tag == HooksTag)
    {
        onHooksResult(outcome);
        return;
    }
    if (tag == AuthTag)
    {
        onAuthResult(outcome, out);
        return;
    }
    if (isAuthTag(tag))
    {
        onSourceAuthResult(tag, outcome, out);
        return;
    }
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

    if (parsed->list == WorktreesList)
    {
        // Not part of the poll's outcome: a failing git does not slow the forge down.
        if (ok)
        {
            state->worktrees = worktreeEntries(outcome.stdoutText);
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
    auto const query = issueQueryIndex(parsed->list);
    auto const label = [&] {
        if (parsed->list == PrsList)
            return std::string("the pull request list");
        if (parsed->list == DefaultBranchList)
            return std::string("the default branch");
        if (query && *query < repo->issueQueries.size())
            return std::format("the issue list \"{}\"", repo->issueQueries[*query].text);
        return std::string(parsed->list);
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
    else if (parsed->list == PrsList)
    {
        if (auto listed = provider->changeRequests(outcome.stdoutText, extent))
        {
            lines = listNotices(repo->name, label, *listed);
            state->changeRequests = std::move(listed->items);
        }
        else
            unread();
    }
    else if (parsed->list == DefaultBranchList)
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
    // Issues found by several queries are offered once.
    auto issues = std::vector<Issue> {};
    for (auto const& found: state->issuesByQuery | std::views::join)
        if (std::ranges::none_of(issues, [&](Issue const& known) { return known.url == found.url; }))
            issues.push_back(found);
    state->unclaimed = unclaimed(std::move(issues), state->changeRequests);
    onListAnswered(out);
}

void Engine::onListAnswered(std::vector<Effect>& out)
{
    if (_pollOutstanding > 0)
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
    // A blocked or done agent makes no tool calls: it writes no board.
    auto const after = static_cast<uint64_t>(_config->statusBoardAfter().count());
    if (after == 0 || !std::holds_alternative<Running>(run.state) || !run.pane
        || _boardsFound.contains(run.id))
        return std::nullopt;
    return later(run.runningSinceMs, after);
}

void Engine::readBoards(std::vector<Effect>& out)
{
    for (auto const& run: std::span(_runs).first(_insideCount))
        if (auto const due = boardDueAt(run); due && _monoMs >= *due)
            out.emplace_back(ReadFile { .tag = std::format("{}{}", run.id, BoardTagSuffix),
                                        .path = std::format("{}/STATUS.md", run.worktree) });
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
                                .path = std::format("{}/STATUS.md", run.worktree) });
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
