// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
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
#include <orchestrator/core/Provider.hpp>
#include <orchestrator/core/Sanitize.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    using engine::detail::BoardTagSuffix;
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
        repo.orphans = repo.worktrees.empty() || !_config
                           ? std::vector<WorktreeEntry> {}
                           : orchestrator::orphans(_runs, _config->worktreeRoot(), repo.worktrees);
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
    auto const* const provider = findProvider(first.forge);
    if (provider != nullptr && _auth == AuthState::LoggedIn && _authCheck == AuthCheck::Skip)
        issueLists(out);
    else if (provider != nullptr
             && issuePoll(std::string(AuthTag), provider->authCheck, {}, first.path, out) == Issued::Yes)
        ++_pollOutstanding;
    // Nothing handed to the host: the poll failed, and the next one is still scheduled, backed off.
    if (_pollOutstanding == 0)
    {
        _pollResult = PollResult::Failed;
        onListAnswered(out);
    }
    // The worktree lists need no forge: cleanup works while `gh` is logged out.
    for (auto const& repo: _config->repos())
    {
        auto tag = std::format("{}{}/{}", PollTagPrefix, repo.name, WorktreesList);
        if (!isPending(tag))
            static_cast<void>(issuePoll(std::move(tag), WorktreeListCommand, {}, repo.path, out));
    }
}

void Engine::issueLists(std::vector<Effect>& out)
{
    for (auto const& repo: _config->repos())
    {
        auto const* const provider = findProvider(repo.forge);
        auto const* const state = findRepoState(repo.name);
        if (provider == nullptr || state == nullptr)
            continue;
        auto const tagFor = [&](std::string_view list) {
            return std::format("{}{}/{}", PollTagPrefix, repo.name, list);
        };
        // Only a command handed to the host is waited for.
        auto const count = [&](Issued issued) {
            if (issued == Issued::Yes)
                ++_pollOutstanding;
        };
        if (!repo.base && !state->defaultBranch)
            count(issuePoll(tagFor(DefaultBranchList), provider->defaultBranch, {}, repo.path, out));
        count(issuePoll(tagFor(PrsList), provider->listChangeRequests, {}, repo.path, out));
        auto index = std::size_t { 0 };
        for (auto const& query: repo.issueQueries)
            count(issuePoll(tagFor(std::format("{}{}", IssuesList, index++)),
                            provider->listIssues,
                            { Argument { .name = "query", .value = query } },
                            repo.path,
                            out));
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
        lines.push_back(judgement.verdict == Verdict::CwdMissing
                            ? line(std::format("{}: {} does not exist", first.name, first.path))
                            : line(std::format("gh is not logged in: {}", judgement.why)));
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
    auto const judgement = judge(outcome);
    auto const ok = judgement.verdict == Verdict::Ok;

    auto const parsed = parsePollTag(tag);
    auto* const state = parsed ? findRepoState(parsed->repo) : nullptr;
    auto const* const repo = parsed ? _config->findRepo(parsed->repo) : nullptr;
    auto const* const provider = repo != nullptr ? findProvider(repo->forge) : nullptr;
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
            return std::format("the issue list \"{}\"", repo->issueQueries[*query]);
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
    else if (query && *query < state->issuesByQuery.size())
    {
        if (auto listed = provider->issues(outcome.stdoutText, extent))
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
        // kept run's reason with "not known": it waits for a poll that knows it.
        else if (finished->fate == Fate::Kept && !unknownCheckout(run))
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

// }}}

} // namespace orchestrator
