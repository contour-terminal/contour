// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Prompt.hpp>

namespace orchestrator
{

namespace
{
    /// Whether @p a and @p b name the same item.
    [[nodiscard]] bool sameItem(ItemRef const& a, ItemRef const& b) noexcept
    {
        return std::visit(
            Overloaded {
                [](ChangeRequestRef const& x, ChangeRequestRef const& y) { return x.number == y.number; },
                [](IssueRef const& x, IssueRef const& y) { return x.number == y.number; },
                [](JiraRef const& x, JiraRef const& y) { return x.key == y.key; },
                [](auto const&, auto const&) { return false; },
            },
            a,
            b);
    }

    /// The kind of task @p item takes.
    [[nodiscard]] AppliesTo kindOf(ItemRef const& item) noexcept
    {
        return std::holds_alternative<ChangeRequestRef>(item) ? AppliesTo::PullRequest : AppliesTo::Issue;
    }
} // namespace

void Engine::dispatch(Dispatch const& request, std::vector<Effect>& out)
{
    auto prepared = prepare(request);
    if (!prepared)
    {
        out.emplace_back(DispatchRefused { .repo = request.repo, .reason = prepared.error() });
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
        out.emplace_back(DispatchRefused { .repo = request.repo, .reason = DispatchRefusal::BadPrompt });
        return;
    }
    apply(id, std::move(*effects), out);
}

std::expected<Run, DispatchRefusal> Engine::prepare(Dispatch const& request) const
{
    if (!_config)
        return std::unexpected { DispatchRefusal::NotConfigured };
    auto const* const repo = _config->findRepo(request.repo);
    if (repo == nullptr)
        return std::unexpected { DispatchRefusal::UnknownRepo };
    auto const* const task = _config->findTask(request.task);
    if (task == nullptr || task->appliesTo != kindOf(request.item))
        return std::unexpected { DispatchRefusal::UnknownTask };
    auto const& model = request.model.empty() ? task->model : request.model;
    if (!std::ranges::contains(_config->models(), model))
        return std::unexpected { DispatchRefusal::UnknownModel };

    // Only active runs count: a Finished entry is history, and Done is active until the user ends it.
    auto active = _runs | std::views::filter(countsAsActive);
    if (std::ranges::any_of(active, [&](Run const& run) {
            return run.repo == repo->name && sameItem(run.item, request.item);
        }))
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
    auto const id = runId(request.item, stampFromUnixMs(request.wallMs, _host.utcOffsetMinutes));
    if (!prompt || !id)
        return std::unexpected { DispatchRefusal::BadPrompt };
    if (std::ranges::contains(_runs, id->text(), &Run::id))
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
                     .state = Preparing { PrepareStep::Fetch },
                     .lastStatus = std::nullopt,
                     .failure = std::nullopt,
                     .startedWallMs = request.wallMs,
                     .attempt = 0,
                     .runningSinceMs = 0,
                     .changedMonoMs = 0 };
    if (!isStorable(run))
        return std::unexpected { DispatchRefusal::BadPrompt };
    return run;
}

} // namespace orchestrator
