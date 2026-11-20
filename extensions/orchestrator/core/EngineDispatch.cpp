// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Prompt.hpp>
#include <orchestrator/core/Reconcile.hpp>

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

std::size_t Engine::activeRuns() const noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(_runs, countsAsActive));
}

std::vector<Argument> agentPaneArguments(Run const& run)
{
    return { { .name = "model", .value = run.model },
             { .name = "session", .value = run.session },
             { .name = "prompt", .value = run.prompt } };
}

std::expected<DispatchPreview, DispatchRefusal> Engine::preview(Dispatch const& request) const
{
    if (!_config)
        return std::unexpected { DispatchRefusal::NotConfigured };
    auto const* const repo = _config->findRepo(request.repo);
    if (repo == nullptr)
        return std::unexpected { DispatchRefusal::UnknownRepo };
    auto const* const task = _config->findTask(request.task);
    if (task == nullptr || task->appliesTo != appliesToOf(request.item))
        return std::unexpected { DispatchRefusal::UnknownTask };
    auto const& model = request.model.empty() ? task->model : request.model;
    if (!std::ranges::contains(_config->models(), model))
        return std::unexpected { DispatchRefusal::UnknownModel };

    // Only active runs count: a Finished entry is history, and Done is active until the user ends it.
    auto active = _runs | std::views::filter(countsAsActive);
    if (activeRunOn(repo->name, request.item) != nullptr)
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
    if (auto const* const changeRequest = std::get_if<ChangeRequestRef>(&request.item))
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

    auto prompt = buildPrompt(task->prompt, request.url, request.note);
    // A run of the item dispatched in the same second, since finished, still holds the plain id, and so may a
    // worktree no run accounts for, or its branch, which `git worktree add -b` would refuse: the new run
    // takes the next ordinal (no active run of the item exists here; that was refused above).
    auto const isTaken = [&](RunId const& candidate) {
        auto const path = worktreePath(_config->worktreeRoot(), repo->name, candidate);
        auto const branch = branchName(candidate);
        return std::ranges::contains(_runs, candidate.text(), &Run::id)
               || std::ranges::any_of(state->worktrees, [&](WorktreeEntry const& worktree) {
                      return worktree.path == path || worktree.branch == branch;
                  });
    };
    auto const stamp = stampFromUnixMs(request.wallMs, _host.utcOffsetMinutes);
    auto id = runId(request.item, stamp);
    for (auto const ordinal: std::views::iota(uint32_t { 2 }, MaxRunsPerSecond + 1))
    {
        if (!id || !isTaken(*id))
            break;
        id = runId(request.item, stamp, ordinal);
    }
    if (!prompt || !id)
        return std::unexpected { DispatchRefusal::BadPrompt };
    if (isTaken(*id))
        return std::unexpected { DispatchRefusal::AlreadyRunning };

    auto run = Run { .id = std::string(id->text()),
                     .repo = repo->name,
                     .repoPath = repo->path,
                     .forge = repo->forge,
                     .remote = repo->remote,
                     .base = std::move(*base),
                     .item = request.item,
                     .url = request.url,
                     .task = task->id,
                     .model = model,
                     .prompt = std::move(*prompt),
                     .session = request.session,
                     .branch = branchName(*id),
                     .worktree = worktreePath(_config->worktreeRoot(), repo->name, *id),
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
    auto const* const pane = findPaneTemplate(AgentPaneTemplate);
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
