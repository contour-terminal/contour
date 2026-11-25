// SPDX-License-Identifier: Apache-2.0
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/WorkItems.hpp>

namespace orchestrator
{

std::vector<RepoWork> Engine::repoWork() const
{
    auto repos = std::vector<RepoWork> {};
    if (!_config)
        return repos;
    // In configuration order: a ticket two repositories list is the first's.
    repos.reserve(_config->repos().size());
    for (auto const& repo: _config->repos())
        if (auto const* const state = findRepoState(repo.name))
            repos.push_back(RepoWork {
                .repo = state->name, .changeRequests = state->changeRequests, .issues = state->issues });
    return repos;
}

std::vector<WorkItem> Engine::workItems() const
{
    if (!_config)
        return {};
    auto const repos = repoWork();
    auto const projects = jiraProjectsOf(_config->repos(), repos);
    // Qualified: this member hides the free function of the same name.
    return orchestrator::workItems(WorkInput {
        .repos = repos, .linkedTickets = _linkedTickets, .jiraProjects = projects, .runs = _runs });
}

} // namespace orchestrator
