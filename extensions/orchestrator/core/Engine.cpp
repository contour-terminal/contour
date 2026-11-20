// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Engine.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/EngineDetail.hpp>
#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Sanitize.hpp>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace engine::detail
{
    Judgement judge(ProcessOutcome const& outcome)
    {
        switch (outcome.start)
        {
            case StartResult::CwdMissing:
                return { .verdict = Verdict::CwdMissing, .why = "its directory does not exist" };
            case StartResult::Refused: {
                auto why =
                    displayText(std::string_view(outcome.stderrText).substr(0, outcome.stderrText.find('\n')),
                                MaxEntryChars);
                return { .verdict = Verdict::Failed,
                         .why = why.empty() ? std::string("the host refused it") : std::move(why) };
            }
            case StartResult::Started: break;
        }
        if (outcome.completion == Completion::TimedOut)
            return { .verdict = Verdict::Failed, .why = "it timed out" };
        if (outcome.exitCode != 0)
        {
            auto const firstLine =
                std::string_view(outcome.stderrText).substr(0, outcome.stderrText.find('\n'));
            auto why = displayText(firstLine, MaxEntryChars);
            if (why.empty())
                why = outcome.exitCode ? std::format("it exited with {}", *outcome.exitCode)
                                       : std::string("it exited without a code");
            return { .verdict = Verdict::Failed, .why = std::move(why) };
        }
        if (outcome.extent == OutputExtent::Truncated)
            return { .verdict = Verdict::Cut, .why = "its output was cut at the host's limit" };
        return { .verdict = Verdict::Ok, .why = {} };
    }
} // namespace engine::detail

namespace
{
    using engine::detail::BoardTagSuffix;
    using engine::detail::BoardViewTagSuffix;
    using engine::detail::judge;
    using engine::detail::later;
    using engine::detail::PollTagPrefix;
    using engine::detail::TranscriptTagSuffix;
    using engine::detail::Verdict;

    /// What a stored run's key starts with.
    constexpr auto RunKeyPrefix = "run/"sv;

    /// The commands that delete from a worktree: never run while a tab of its run is listed.
    constexpr auto DeletingCommands = std::array { "git-clean"sv, "git-worktree-remove"sv };

    /// Why a deleting command was held back: a tab of the run is listed.
    constexpr auto TabStillListed = "a tab of the run is still open"sv;
    /// Why a deleting command was held back: no listing was known yet, so none can say that no tab is open.
    constexpr auto TabsNotKnown = "the open tabs are not known yet"sv;

    /// Whether @p effect makes the run await a tagged host call.
    [[nodiscard]] bool awaitsCall(RunEffect const& effect) noexcept
    {
        return std::holds_alternative<RunDeclared>(effect) || std::holds_alternative<ReadGitFile>(effect);
    }

    [[nodiscard]] std::expected<std::string_view, FileReadError> viewOf(
        std::expected<std::string, FileReadError> const& content)
    {
        if (!content)
            return std::unexpected { content.error() };
        return std::string_view { *content };
    }
} // namespace

std::optional<std::string> callTag(Run const& run)
{
    auto tag = stepTag(run);
    if (!tag)
        return std::nullopt;
    return std::format("{}#{}", *tag, run.attempt);
}

std::string runKey(std::string_view runId)
{
    return std::format("{}{}", RunKeyPrefix, runId);
}

Engine::Engine(std::optional<Config> config, HostInfo host, uint64_t monoMs, uint64_t wallMs):
    _config { std::move(config) },
    _repositoryNames { _config ? std::ranges::to<std::vector<std::string>>(
                                     _config->repos() | std::views::transform(&Repo::name))
                               : std::vector<std::string> {} },
    _host { std::move(host) },
    _nextPollMs { monoMs },
    _monoMs { monoMs },
    _wallMs { wallMs }
{
    if (!_config)
        return;
    for (auto const& repo: _config->repos())
        _repos.push_back(
            RepoState { .name = repo.name,
                        .changeRequests = {},
                        .issuesByQuery = std::vector<std::vector<Issue>>(repo.issueQueries.size()),
                        .unclaimed = {},
                        .defaultBranch = std::nullopt,
                        .worktrees = {},
                        .orphans = {},
                        .unknownCheckout = CheckoutUnknown::Pending,
                        .worktreeListFailures = 0 });
}

Engine::Started Engine::start(StartContext context)
{
    auto config = Config::parse(context.configJson, context.host, context.sources);
    auto engine = Engine(config ? std::optional { std::move(*config) } : std::nullopt,
                         context.host,
                         std::max<uint64_t>(context.monoMs, 1),
                         context.wallMs);
    auto out = std::vector<Effect> {};
    if (!config)
        engine._startProblems.push_back(message(config.error(), context.sources));
    engine.loadStore(context.stored);
    if (engine._config)
    {
        // The calls the stored runs await, before reconciling moves any run on.
        auto awaited = std::vector<std::string> {};
        for (auto const& run: std::span(engine._runs).first(engine._insideCount))
            if (auto tag = callTag(run))
                awaited.push_back(std::move(*tag));
        engine.reconcileWith(context.panes, engine._monoMs, context.paneListing, out);
        engine.resolveStaleWork(awaited, context, out);
        // A run stored waiting for a main checkout that needs no listing, such as one of a repository no
        // longer configured, goes on now.
        engine.resumeCheckoutWaits(out);
        engine.startOrResumePoll(context, out);
    }
    engine.endStep(out, engine._revision + 1);
    engine._answeredAtStart.clear();
    return Started { .engine = std::move(engine), .effects = std::move(out) };
}

std::vector<Effect> Engine::step(Input const& input)
{
    auto out = std::vector<Effect> {};
    auto const revision = _revision;
    std::visit(
        [&](auto const& arrived) {
            if constexpr (requires { arrived.monoMs; })
                _monoMs = std::max(_monoMs, arrived.monoMs);
        },
        input);
    // A reply that arrives after its deadline still counts: only the calls it does not answer are abandoned.
    auto const* const reply = std::get_if<ProcessFinished>(&input);
    if (_config)
    {
        abandonOverdueCalls(reply != nullptr ? std::string_view { reply->tag } : std::string_view {}, out);
        auto const answeredRun =
            std::visit(Overloaded {
                           [](PaneOpenedFor const& opened) { return std::string_view { opened.run }; },
                           [](PaneOpenFailedFor const& failed) { return std::string_view { failed.run }; },
                           [](auto const&) { return std::string_view {}; },
                       },
                       input);
        abandonOverduePaneOpenings(answeredRun, out);
    }
    std::visit(
        Overloaded {
            [&](Wake const& wake) { _wallMs = wake.wallMs; },
            [&](ProcessFinished const& finished) { onProcessFinished(finished, out); },
            [&](FileRead const& read) { onFileRead(read, out); },
            [&](PanesListed const& listed) {
                if (!_config)
                    return;
                _monoMs = std::max(_monoMs, listed.requestedMonoMs);
                reconcileWith(listed.panes, listed.requestedMonoMs, listed.listing, out);
            },
            [&](PaneOpenedFor const& opened) {
                feed(opened.run, PaneOpened { .pane = opened.pane, .monoMs = _monoMs }, out);
            },
            [&](PaneOpenFailedFor const& failed) { feed(failed.run, PaneOpenFailed { failed.why }, out); },
            [&](Dispatch const& request) { dispatch(request, out); },
            [&](RetryRemovalOf const& retry) {
                // Its checks run git against a configured repository: with a bad configuration nothing runs,
                // as for a dispatch, and a run outside the configuration is never tried again.
                if (auto const* const run = findRun(retry.run);
                    run != nullptr && scopeOf(*run) == RunScope::Inside)
                    feed(retry.run, RetryRemoval {}, out);
            },
            [&](EndRun const& end) {
                // The user may end any stored run, one outside the configuration too: its cleanup then works
                // from what the run stored.
                if (auto const* const run = findRun(end.run); run != nullptr && !isTracked(*run))
                {
                    auto const at = std::ranges::find(_runs, end.run, &Run::id);
                    std::rotate(_runs.begin() + static_cast<std::ptrdiff_t>(_insideCount), at, at + 1);
                    ++_insideCount;
                }
                feed(end.run, EndByUser {}, out);
            },
            [&](DismissRun const& dismiss) {
                // Dismissing runs nothing, it only forgets a removed run: one outside the configuration that
                // the engine leaves alone may be dismissed too.
                if (auto* const run = findRun(dismiss.run); run != nullptr && !isTracked(*run))
                {
                    auto const id = run->id;
                    apply(id, advance(*run, Dismiss {}, contextFor(*run)), out);
                }
                else
                    feed(dismiss.run, Dismiss {}, out);
            },
            [&](MarkRunSeen const& seen) { feed(seen.run, MarkSeen {}, out); },
            [&](ReadTranscript const& request) { readTranscript(request, out); },
            [&](ShowBoard const& request) { showBoard(request, out); },
            [&](HideBoard const& request) { hideBoard(request.tab); },
            [&](TabClosed const& closed) { closeTab(closed.tab); },
            [&](RefreshNow const&) {
                if (!_config)
                    return;
                if (_pollOutstanding == 0)
                    startPoll(out);
                checkHooks(out);
            },
        },
        input);
    // Work that fell due is done on any input, so that no wake is ever asked for in the past; the status
    // boards past their threshold are read on wakes only.
    if (_config)
    {
        if (_pollOutstanding == 0 && _monoMs >= _nextPollMs)
            startPoll(out);
        if (std::holds_alternative<Wake>(input))
            readBoards(out);
        refreshShownBoards(out);
    }
    endStep(out, revision);
    return out;
}

std::span<ChangeRequest const> Engine::changeRequests(std::string_view repo) const noexcept
{
    auto const* const state = findRepoState(repo);
    return state != nullptr ? std::span<ChangeRequest const> { state->changeRequests }
                            : std::span<ChangeRequest const> {};
}

std::span<Issue const> Engine::unclaimedIssues(std::string_view repo) const noexcept
{
    auto const* const state = findRepoState(repo);
    return state != nullptr ? std::span<Issue const> { state->unclaimed } : std::span<Issue const> {};
}

std::span<WorktreeEntry const> Engine::orphans(std::string_view repo) const noexcept
{
    auto const* const state = findRepoState(repo);
    return state != nullptr ? std::span<WorktreeEntry const> { state->orphans }
                            : std::span<WorktreeEntry const> {};
}

std::optional<std::string_view> Engine::mainCheckoutOf(std::string_view repo) const noexcept
{
    auto const* const state = findRepoState(repo);
    if (state == nullptr)
        return std::nullopt;
    return mainCheckout(state->worktrees);
}

// {{{ runs

Run* Engine::findRun(std::string_view id) noexcept
{
    auto const run = std::ranges::find(_runs, id, &Run::id);
    return run != _runs.end() ? &*run : nullptr;
}

RunScope Engine::scopeOf(Run const& run) const noexcept
{
    // A refused configuration says nothing about where a run belongs.
    if (!_config)
        return RunScope::Unknown;
    auto const* const repo = _config->findRepo(run.repo);
    if (repo == nullptr || repo->path != run.repoPath)
        return RunScope::Outside;
    // Exactly where a dispatch under this configuration puts it.
    return run.worktree == worktreePath(_config->worktreeRoot(), run.repo, run.id) ? RunScope::Inside
                                                                                   : RunScope::Outside;
}

StepContext Engine::contextFor(Run const& run) const noexcept
{
    // A repository no longer configured is listed by nobody: its stored directory is its main checkout.
    auto const* const state = findRepoState(run.repo);
    auto const* const repo = _config ? _config->findRepo(run.repo) : nullptr;
    auto main = std::optional<std::string_view> {};
    if (state != nullptr && repo != nullptr && repo->path == run.repoPath)
        main = mainCheckout(state->worktrees);
    else if (repo == nullptr)
        main = std::string_view { run.repoPath };
    auto const unknown = state != nullptr && repo != nullptr && repo->path == run.repoPath
                             ? state->unknownCheckout
                             : CheckoutUnknown::Unavailable;
    return StepContext {
        .mainCheckout = main,
        .unknownCheckout = unknown,
        .wallMs = _wallMs,
        .monoMs = _monoMs,
        .providers = _config ? _config->sources().providers : providers(),
    };
}

std::optional<CheckoutUnknown> Engine::unknownCheckout(Run const& run) const noexcept
{
    auto const context = contextFor(run);
    if (context.mainCheckout)
        return std::nullopt;
    return context.unknownCheckout;
}

void Engine::loadStore(std::span<StoredEntry const> stored)
{
    for (auto const& entry: stored)
    {
        if (!entry.key.starts_with(RunKeyPrefix))
            continue;
        auto run = runFromJson(entry.value);
        if (!run || runKey(run->id) != entry.key)
        {
            _startProblems.push_back(std::format("{}: the stored run cannot be read; it is kept as it is",
                                                 displayText(entry.key, MaxEntryChars)));
            continue;
        }
        // Monotonic times start again with this instance: a board is due counting from now.
        run->runningSinceMs = _monoMs;
        _runs.push_back(std::move(*run));
    }
    auto const outside = std::ranges::stable_partition(
        _runs, [this](Run const& run) { return scopeOf(run) == RunScope::Inside || isEndedByYou(run); });
    _insideCount = static_cast<std::size_t>(outside.begin() - _runs.begin());
}

bool Engine::isEndedByYou(Run const& run) noexcept
{
    // A run the user ended outside the configuration is worked on again after a restart, so that it completes
    // and its entry can be dismissed and purged.
    if (auto const* const ending = std::get_if<Ending>(&run.state))
        return ending->cause == Ended::EndedByYou;
    if (auto const* const finished = std::get_if<Finished>(&run.state))
        return finished->ended == Ended::EndedByYou;
    return false;
}

bool Engine::isTracked(Run const& run) const noexcept
{
    return std::cmp_less(&run - _runs.data(), _insideCount);
}

void Engine::resolveStaleWork(std::span<std::string const> awaited,
                              StartContext const& context,
                              std::vector<Effect>& out)
{
    auto ids = std::vector<std::string> {};
    for (auto const& run: std::span(_runs).first(_insideCount))
        ids.push_back(run.id);
    for (auto const& id: ids)
    {
        auto const* const run = findRun(id);
        if (run == nullptr)
            continue;
        // Only a call the run awaited when it was read: one reconciling just made is this instance's own.
        auto const tag = callTag(*run);
        if (!tag || !std::ranges::contains(awaited, *tag))
            continue;
        if (std::ranges::contains(context.inFlight, *tag))
        {
            // Its timeout class is not known here: the longer one.
            _inFlight.push_back(InFlightCall {
                .tag = *tag,
                .deadlineMs =
                    later(_monoMs, static_cast<uint64_t>((LongTimeout + CallDeadlineMargin).count())) });
            continue;
        }
        auto const finished = std::ranges::find(context.finished, *tag, &FinishedResult::tag);
        if (finished != context.finished.end())
            onRunResult(id, finished->outcome, out);
        else
            feed(id, Lost {}, out);
    }
}

void Engine::reconcileWith(std::span<PaneFact const> panes,
                           uint64_t requestedMonoMs,
                           PaneListing listing,
                           std::vector<Effect>& out)
{
    // Nothing listed tells nothing: a run whose pane is not known to be gone is never ended on it.
    if (listing == PaneListing::Unknown)
        return;
    _panes.assign(panes.begin(), panes.end());
    _paneListing = listing;
    auto const decisions =
        reconcile(std::span<Run const>(_runs).first(_insideCount), _panes, requestedMonoMs, listing);
    for (auto const& decision: decisions)
        std::visit(Overloaded {
                       [&](Adopt const& adopt) {
                           feed(adopt.run, PaneOpened { .pane = adopt.pane, .monoMs = _monoMs }, out);
                       },
                       [&](StatusChanged const& changed) {
                           feed(changed.run, StatusReported { changed.status }, out);
                       },
                       [&](ClosePane const& close) { out.emplace_back(close); },
                       [&](PaneWentAway const& away) { feed(away.run, PaneGone {}, out); },
                   },
                   decision);
}

void Engine::feed(std::string_view runId, RunInput const& input, std::vector<Effect>& out)
{
    auto* const run = findRun(runId);
    if (run == nullptr || !isTracked(*run))
        return;
    auto const id = run->id;
    apply(id, advance(*run, input, contextFor(*run)), out);
}

void Engine::apply(std::string_view runId, std::vector<RunEffect> effects, std::vector<Effect>& out)
{
    auto* const run = findRun(runId);
    if (run == nullptr || effects.empty())
        return;
    ++_revision;
    // Counted before the run is stored, so that the stored run names the call it awaits.
    if (std::ranges::any_of(effects, awaitsCall))
        ++run->attempt;
    auto forgotten = false;
    auto held = std::optional<std::string_view> {};
    for (auto& effect: effects)
        std::visit(
            Overloaded {
                [&](Persist const&) {
                    out.emplace_back(StoreSet { .key = runKey(run->id), .value = toJson(*run) });
                },
                [&](Forget const&) {
                    out.emplace_back(StoreRemove { .key = runKey(run->id) });
                    forgotten = true;
                },
                [&](RunDeclared& declared) {
                    auto const* const command = findCommand(declared.name);
                    if (command == nullptr)
                        return;
                    if (std::ranges::contains(DeletingCommands, command->name))
                    {
                        if (_paneListing == PaneListing::Unknown)
                            held = TabsNotKnown;
                        else if (std::ranges::contains(_panes, run->id, &PaneFact::tag))
                            held = TabStillListed;
                        if (held)
                            return;
                    }
                    issue(RunCommand { .tag = callTag(*run).value_or(run->id),
                                       .command = command->name,
                                       .arguments = std::move(declared.arguments),
                                       .cwd = command->root == Root::Repos ? run->repoPath : run->worktree },
                          Priority::Run,
                          out);
                },
                [&](ReadGitFile const&) {
                    out.emplace_back(ReadFile { .tag = callTag(*run).value_or(run->id),
                                                .path = std::format("{}/.git", run->worktree) });
                },
                [&](OpenAgentPane const&) {
                    auto const* const pane = findPaneTemplate(AgentPaneTemplate);
                    if (pane == nullptr)
                        return;
                    std::erase_if(_paneOpenings,
                                  [&](InFlightCall const& opening) { return opening.tag == run->id; });
                    _paneOpenings.push_back(InFlightCall {
                        .tag = run->id,
                        .deadlineMs =
                            later(_monoMs,
                                  static_cast<uint64_t>((PaneOpenTimeout + CallDeadlineMargin).count())) });
                    out.emplace_back(OpenPane { .run = run->id,
                                                .paneTemplate = pane->name,
                                                .arguments = agentPaneArguments(*run),
                                                .cwd = run->worktree,
                                                .title = run->id,
                                                .nearTab = run->nearTab });
                },
                [&](CloseAgentPane const&) {
                    auto panes = std::vector<std::string> {};
                    if (run->pane)
                        panes.push_back(*run->pane);
                    for (auto const& listed: _panes)
                        if (listed.tag == run->id && !std::ranges::contains(panes, listed.pane))
                            panes.push_back(listed.pane);
                    for (auto& pane: panes)
                        out.emplace_back(ClosePane { .pane = std::move(pane) });
                },
            },
            effect);
    if (forgotten)
        forget(runId);
    else if (held)
        feed(runId, CommandFailed { *held }, out);
}

void Engine::forget(std::string_view runId)
{
    auto const run = std::ranges::find(_runs, runId, &Run::id);
    if (run == _runs.end())
        return;
    if (std::cmp_less(run - _runs.begin(), _insideCount))
        --_insideCount;
    if (auto const found = _boardsFound.find(runId); found != _boardsFound.end())
        _boardsFound.erase(found);
    std::erase_if(_transcripts, [&](TranscriptView const& view) { return view.run == runId; });
    std::erase_if(_transcriptShown, [&](auto const& shown) { return shown.second == runId; });
    std::erase_if(_shownBoards, [&](ShownBoard const& shown) { return shown.board.run == runId; });
    _runs.erase(run);
}

void Engine::onRunResult(std::string_view runId, ProcessOutcome const& outcome, std::vector<Effect>& out)
{
    auto const judgement = judge(outcome);
    switch (judgement.verdict)
    {
        case Verdict::Ok: feed(runId, CommandOk { outcome.stdoutText }, out); return;
        // A step reads all of what its command printed: a cut output is no answer.
        case Verdict::Cut:
        case Verdict::Failed: feed(runId, CommandFailed { judgement.why }, out); return;
        case Verdict::CwdMissing: feed(runId, CwdMissing {}, out); return;
    }
}

void Engine::onProcessFinished(ProcessFinished const& finished, std::vector<Effect>& out)
{
    // A reply to a call this instance does not await, such as an earlier attempt's, is dropped.
    auto const inFlight = std::ranges::find(_inFlight, finished.tag, &InFlightCall::tag);
    if (inFlight == _inFlight.end())
        return;
    _inFlight.erase(inFlight);
    if (finished.tag.starts_with(PollTagPrefix))
    {
        onPollResult(finished.tag, finished.outcome, out);
        return;
    }
    if (finished.tag.ends_with(TranscriptTagSuffix))
    {
        onTranscriptResult(
            std::string_view(finished.tag).substr(0, finished.tag.size() - TranscriptTagSuffix.size()),
            finished.outcome,
            out);
        return;
    }
    auto const run = std::ranges::find_if(std::span(_runs).first(_insideCount), [&](Run const& candidate) {
        return callTag(candidate) == finished.tag;
    });
    if (run == std::span(_runs).first(_insideCount).end())
        return;
    auto const id = run->id;
    onRunResult(id, finished.outcome, out);
}

void Engine::onFileRead(FileRead const& read, std::vector<Effect>& out)
{
    if (read.tag.ends_with(BoardTagSuffix))
    {
        onBoardRead(std::string_view(read.tag).substr(0, read.tag.size() - BoardTagSuffix.size()), read);
        return;
    }
    if (read.tag.ends_with(BoardViewTagSuffix))
    {
        onShownBoardRead(std::string_view(read.tag).substr(0, read.tag.size() - BoardViewTagSuffix.size()),
                         read);
        return;
    }
    auto const inside = std::span(_runs).first(_insideCount);
    auto const run =
        std::ranges::find_if(inside, [&](Run const& candidate) { return callTag(candidate) == read.tag; });
    if (run == inside.end())
        return;
    auto const id = run->id;
    feed(id, GitFileRead { viewOf(read.content) }, out);
}

// }}}
// {{{ the queue

void Engine::issue(RunCommand command, Priority priority, std::vector<Effect>& out)
{
    auto const isRun = [](Queued const& queued) {
        return queued.priority == Priority::Run;
    };
    auto const slotFree = _inFlight.size() < MaxInFlight;
    if (priority == Priority::Run)
    {
        if (slotFree && std::ranges::none_of(_queue, isRun))
        {
            emit(std::move(command), out);
            return;
        }
        // Behind the other runs' commands, ahead of every poll.
        auto const firstPoll = std::ranges::find_if_not(_queue, isRun);
        _queue.insert(firstPoll, Queued { .priority = priority, .command = std::move(command) });
        return;
    }
    if (slotFree && _queue.empty())
    {
        emit(std::move(command), out);
        return;
    }
    _queue.push_back(Queued { .priority = priority, .command = std::move(command) });
}

void Engine::drainQueue(std::vector<Effect>& out)
{
    while (_inFlight.size() < MaxInFlight && !_queue.empty())
    {
        auto queued = std::move(_queue.front());
        _queue.pop_front();
        // A run that moved on while its command waited does not await it any more.
        if (queued.priority == Priority::Run && std::ranges::none_of(_runs, [&](Run const& run) {
                return callTag(run) == queued.command.tag;
            }))
            continue;
        emit(std::move(queued.command), out);
    }
}

void Engine::emit(RunCommand command, std::vector<Effect>& out)
{
    auto const* const decl = findCommand(command.command);
    auto const timeout = decl != nullptr && decl->timeout == Timeout::Short ? ShortTimeout : LongTimeout;
    _inFlight.push_back(InFlightCall {
        .tag = command.tag,
        .deadlineMs = later(_monoMs, static_cast<uint64_t>((timeout + CallDeadlineMargin).count())) });
    out.emplace_back(std::move(command));
}

void Engine::abandonOverdueCalls(std::string_view answered, std::vector<Effect>& out)
{
    auto overdue = std::vector<std::string> {};
    for (auto const& call: _inFlight)
        if (call.deadlineMs <= _monoMs && call.tag != answered)
            overdue.push_back(call.tag);
    // Through the normal path, so that a run or a poll waiting for it moves on and its slot is freed.
    for (auto& tag: overdue)
        onProcessFinished(
            ProcessFinished { .tag = std::move(tag),
                              .outcome = ProcessOutcome { .start = StartResult::Refused,
                                                          .completion = Completion::Exited,
                                                          .exitCode = std::nullopt,
                                                          .extent = OutputExtent::Whole,
                                                          .stdoutText = {},
                                                          .stderrText = "no reply came in time" },
                              .monoMs = _monoMs },
            out);
}

void Engine::abandonOverduePaneOpenings(std::string_view answeredRun, std::vector<Effect>& out)
{
    auto overdue = std::vector<std::string> {};
    for (auto const& opening: _paneOpenings)
        if (opening.deadlineMs <= _monoMs && opening.tag != answeredRun)
            overdue.push_back(opening.tag);
    std::erase_if(_paneOpenings,
                  [&](InFlightCall const& opening) { return std::ranges::contains(overdue, opening.tag); });
    // The tab may still open: the failure path closes whatever tab is tagged with the run before it touches
    // the worktree.
    for (auto const& runId: overdue)
        feed(runId, PaneOpenFailed { "no reply came in time" }, out);
}

void Engine::forgetSettledPaneOpenings()
{
    std::erase_if(_paneOpenings, [this](InFlightCall const& opening) {
        auto const* const run = findRun(opening.tag);
        auto const* const preparing = run != nullptr ? std::get_if<Preparing>(&run->state) : nullptr;
        return preparing == nullptr || preparing->step != PrepareStep::OpenPane;
    });
}

bool Engine::isInFlight(std::string_view tag) const noexcept
{
    return std::ranges::contains(_inFlight, tag, &InFlightCall::tag);
}

bool Engine::isPending(std::string_view tag) const noexcept
{
    return isInFlight(tag)
           || std::ranges::any_of(_queue, [&](Queued const& queued) { return queued.command.tag == tag; });
}

// }}}
// {{{ the step's end

void Engine::endStep(std::vector<Effect>& out, uint64_t revision)
{
    drainQueue(out);
    forgetSettledPaneOpenings();
    refreshOrphans();
    rebuildProblems();
    if (_config)
    {
        auto summary = summarize(countRuns(_runs, _config->maxAgents(), failingChangeRequests()), _runs);
        if (summary.shortText != _published.shortText || summary.detail != _published.detail)
        {
            out.emplace_back(PublishSummary { .shortText = summary.shortText, .detail = summary.detail });
            _published = std::move(summary);
        }
    }
    if (_revision != revision)
        out.emplace_back(Redraw {});
    auto const wake = nextWake();
    if (wake && (wake != _wakeAt || *wake <= _monoMs))
        out.emplace_back(WakeAt { *wake });
    _wakeAt = wake;
}

std::optional<uint64_t> Engine::nextWake() const
{
    if (!_config)
        return std::nullopt;
    auto wake = std::optional<uint64_t> {};
    auto const consider = [&](uint64_t at) {
        wake = wake ? std::min(*wake, at) : at;
    };
    // While a poll runs, its answers wake the engine; the timer is only a backstop.
    consider(_pollOutstanding == 0 ? _nextPollMs
                                   : later(_monoMs, static_cast<uint64_t>(_config->pollEvery().count())));
    auto const inside = std::span(_runs).first(_insideCount);
    if (std::ranges::any_of(inside, isUnfinished))
        consider(later(_monoMs, static_cast<uint64_t>(ReconcileEvery.count())));
    // A board past its threshold is read on the wakes the unfinished run asks for anyway.
    for (auto const& run: inside)
        if (auto const due = boardDueAt(run); due && *due > _monoMs)
            consider(*due);
    // A shown board is read again on time; refreshShownBoards() leaves each one due ahead of now, but a
    // finished run's, which is read no more.
    for (auto const& shown: _shownBoards)
        if (auto const run = std::ranges::find(_runs, shown.board.run, &Run::id);
            run != _runs.end() && isUnfinished(*run))
            consider(shown.nextReadMs);
    for (auto const& call: _inFlight)
        consider(call.deadlineMs);
    for (auto const& opening: _paneOpenings)
        consider(opening.deadlineMs);
    // Every candidate lies ahead of now: one that did not would make the driver wake the engine at once, and
    // again, so it is not hidden by moving it.
    return wake;
}

void Engine::rebuildProblems()
{
    auto problems = _startProblems;
    if (_config)
        for (auto const& run: std::span(_runs).subspan(_insideCount))
            problems.push_back(displayText(
                std::format(
                    "{}: outside the current configuration ({}); left as it is", run.id, run.worktree),
                MaxSummaryLineChars));
    for (auto const& lines: _pollProblems | std::views::values)
        problems.insert(problems.end(), lines.begin(), lines.end());
    if (_hooks == HooksState::Missing)
        problems.emplace_back(HooksMissingProblem);
    // Orphans are not problems: the dashboard lists them in a section of their own.
    if (problems != _problems)
    {
        _problems = std::move(problems);
        ++_revision;
    }
}

// }}}

} // namespace orchestrator
