// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Agents.hpp>
#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/EngineDetail.hpp>
#include <orchestrator/core/HostPaths.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Permissions.hpp>
#include <orchestrator/core/Prompt.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Sanitize.hpp>

namespace orchestrator
{

namespace
{
    /// The most runs of one item a second names apart (runId()'s ordinal); one more is refused as running.
    constexpr auto MaxRunsPerSecond = uint32_t { 99 };
} // namespace

void Engine::dispatch(Dispatch const& request, std::vector<Effect>& out)
{
    auto prepared = prepare(request);
    if (!prepared)
    {
        out.emplace_back(
            DispatchRefused { .repo = request.repo, .reason = prepared.error(), .nearTab = request.nearTab });
        return;
    }
    auto const id = prepared->id;
    _runs.insert(_runs.begin() + static_cast<std::ptrdiff_t>(_insideCount), std::move(*prepared));
    ++_insideCount;
    auto* const run = findRun(id);
    auto context = contextFor(*run);
    context.wallMs = request.wallMs;
    auto effects = beginRun(*run, context);
    if (!effects)
    {
        forget(id);
        out.emplace_back(DispatchRefused {
            .repo = request.repo, .reason = DispatchRefusal::BadPrompt, .nearTab = request.nearTab });
        return;
    }
    apply(id, std::move(*effects), out);
}

std::expected<Run, DispatchRefusal> Engine::prepare(Dispatch const& request) const
{
    return preview(request).transform([](DispatchPreview preview) { return std::move(preview.run); });
}

Run const* Engine::activeRunOn(std::string_view repo, ItemRef const& item) const noexcept
{
    auto const found = std::ranges::find_if(
        _runs, [&](Run const& run) { return countsAsActive(run) && run.repo == repo && run.item == item; });
    return found != _runs.end() ? &*found : nullptr;
}

std::string_view Engine::reportedMessageOf(Run const& run) const noexcept
{
    auto const found = std::ranges::find(_panes, run.id, &PaneFact::tag);
    return found != _panes.end() ? std::string_view { found->message } : std::string_view {};
}

std::size_t Engine::activeRuns() const noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(_runs, countsAsActive));
}

std::expected<DispatchPreview, DispatchRefusal> Engine::preview(Dispatch const& request) const
{
    if (!_config)
        return std::unexpected { DispatchRefusal::NotConfigured };
    auto const* const repo = _config->findRepo(request.repo);
    if (repo == nullptr)
        return std::unexpected { DispatchRefusal::UnknownRepo };
    // A topic's slug is the engine's to make, from the text: the only part of it in the run's names.
    auto const isTopic = std::holds_alternative<TopicRef>(request.item);
    auto const item = isTopic ? ItemRef { TopicRef { .slug = topicSlug(request.topic) } } : request.item;
    auto const* const task = _config->findTask(request.task);
    if (task == nullptr || task->appliesTo != appliesToOf(item))
        return std::unexpected { DispatchRefusal::UnknownTask };
    auto const& model = request.model.empty() ? task->model : request.model;
    if (!std::ranges::contains(_config->models(), model))
        return std::unexpected { DispatchRefusal::UnknownModel };

    // Only active runs count: a Finished entry is history, and Done is active until the user ends it.
    auto active = _runs | std::views::filter(countsAsActive);
    if (activeRunOn(repo->name, item) != nullptr)
        return std::unexpected { DispatchRefusal::AlreadyRunning };
    if (std::cmp_greater_equal(std::ranges::distance(active), _config->maxAgents()))
        return std::unexpected { DispatchRefusal::AgentLimit };
    if (std::cmp_greater_equal(std::ranges::count(active, repo->name, &Run::repo),
                               _config->maxAgentsPerRepo()))
        return std::unexpected { DispatchRefusal::RepoLimit };

    auto const* const state = findRepoState(repo->name);
    if (state == nullptr)
        return std::unexpected { DispatchRefusal::NotConfigured };
    auto base = std::optional<std::string> {};
    if (auto const* const changeRequest = std::get_if<ChangeRequestRef>(&item))
    {
        // A change request's base is its own, as last listed.
        auto const listed =
            std::ranges::find(state->changeRequests, changeRequest->number, &ChangeRequest::number);
        if (listed != state->changeRequests.end())
            base = listed->base;
    }
    else
        base = repo->base ? repo->base : state->defaultBranch;
    if (!base)
        return std::unexpected { DispatchRefusal::BaseUnknown };

    auto prompt = isTopic ? buildTopicPrompt(task->prompt, request.topic)
                          : buildPrompt(task->prompt, request.url, itemKey(item), request.note);
    // A run of the item dispatched in the same second, since finished, still holds the plain id, and so may a
    // worktree no run accounts for, or its branch, which `git worktree add -b` would refuse: the new run
    // takes the next ordinal (no active run of the item exists here; that was refused above).
    auto const isTaken = [&](RunId const& candidate) {
        auto const path = worktreePath(_config->worktreeRoot(), repo->name, candidate, _config->pathStyle());
        auto const branch = branchName(candidate);
        // git spells a Windows path its own way (`C:/…`): the listing is compared by the host's rule.
        return std::ranges::contains(_runs, candidate.text(), &Run::id)
               || std::ranges::any_of(state->worktrees, [&](WorktreeEntry const& worktree) {
                      return sameHostPath(worktree.path, path, _config->pathStyle())
                             || worktree.branch == branch;
                  });
    };
    auto const stamp = stampFromUnixMs(request.wallMs, _host.utcOffsetMinutes);
    auto id = runId(item, stamp);
    for (auto const ordinal: std::views::iota(uint32_t { 2 }, MaxRunsPerSecond + 1))
    {
        if (!id || !isTaken(*id))
            break;
        id = runId(item, stamp, ordinal);
    }
    if (!prompt || !id)
        return std::unexpected { DispatchRefusal::BadPrompt };
    auto const& worker = _config->workerFor(task->id);
    // An agent that takes no permission profile runs without one, and without a board.
    auto const takesProfile = agentRowOf(worker.agent).settings == SettingsProfile::Taken;
    auto const* const profile = request.permissions.empty() ? &_config->profileFor(task->id)
                                                            : _config->findProfile(request.permissions);
    // A profile no configuration names is refused whatever the agent, rather than ignored unseen.
    if (profile == nullptr)
        return std::unexpected { DispatchRefusal::UnknownProfile };
    auto settings = takesProfile ? settingsJsonOf(*profile) : std::string { EmptySettingsJson };
    if (isTaken(*id))
        return std::unexpected { DispatchRefusal::AlreadyRunning };

    auto run = Run { .id = std::string(id->text()),
                     .repo = repo->name,
                     .repoPath = repo->path,
                     .forge = repo->forge,
                     .remote = repo->remote,
                     .base = std::move(*base),
                     .item = item,
                     .url = isTopic ? std::string {} : request.url,
                     // Shown where an item shows its title: one line, cut like an entry.
                     .topic = isTopic ? displayText(request.topic, MaxEntryChars) : std::string {},
                     .task = task->id,
                     .model = model,
                     .prompt = std::move(*prompt),
                     .board = takesProfile ? _config->boardInstruction() : std::string {},
                     .permissions = takesProfile ? profile->name : std::string {},
                     .settings = std::move(settings),
                     // The implicit worker is stored as none, so that a run reads as it did before workers.
                     .worker = engine::detail::hasImplicitWorkerAlone(*_config) ? std::string {} : worker.id,
                     .agent = worker.agent,
                     .envSet = worker.envSet,
                     .session = request.session,
                     .branch = branchName(*id),
                     .worktree = worktreePath(_config->worktreeRoot(), repo->name, *id, _config->pathStyle()),
                     .pane = std::nullopt,
                     .nearTab = request.nearTab,
                     .state = Preparing { PrepareStep::Fetch },
                     .lastStatus = std::nullopt,
                     .failure = std::nullopt,
                     .startedWallMs = request.wallMs,
                     .attempt = 0,
                     .runningSinceMs = 0,
                     .changedMonoMs = 0 };
    if (!isStorable(run))
        return std::unexpected { DispatchRefusal::BadPrompt };
    auto const* const pane = findPaneTemplate(agentPaneTemplateOf(run));
    if (pane == nullptr)
        return std::unexpected { DispatchRefusal::BadPrompt };
    auto argv = substitute(pane->argv, agentPaneArguments(run));
    if (!argv)
        return std::unexpected { DispatchRefusal::BadPrompt };
    return DispatchPreview { .run = std::move(run),
                             .agentArgv = std::move(*argv),
                             .active = activeRuns(),
                             .maxAgents = _config->maxAgents() };
}

} // namespace orchestrator
