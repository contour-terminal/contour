// SPDX-License-Identifier: Apache-2.0
#include <vthost/AttachPlan.hpp>

#include <algorithm>
#include <ranges>

namespace vthost
{

namespace
{
    /// @return The ids of the workspaces holding at least one tab.
    [[nodiscard]] std::vector<uint64_t> workspacesWithTabs(AttachContext const& context)
    {
        return std::ranges::to<std::vector>(
            context.workspaces
            | std::views::filter([](WorkspaceSummary const& workspace) { return workspace.tabCount > 0; })
            | std::views::transform(&WorkspaceSummary::id));
    }

    /// @return The ids in @p wanted that name an existing workspace, ascending.
    [[nodiscard]] std::vector<uint64_t> existing(std::vector<uint64_t> const& wanted,
                                                 AttachContext const& context)
    {
        return std::ranges::to<std::vector>(
            context.workspaces | std::views::transform(&WorkspaceSummary::id)
            | std::views::filter([&](uint64_t id) { return std::ranges::contains(wanted, id); }));
    }

    /// Every workspace with tabs; with none, the primary workspace gets its first tab.
    [[nodiscard]] AttachPlan takeEverything(FollowScope scope, AttachContext const& context)
    {
        auto follow = workspacesWithTabs(context);
        if (!follow.empty())
            return { .scope = scope, .follow = std::move(follow), .creation = AttachCreation::None };
        return { .scope = scope,
                 .follow = { context.primaryWorkspace },
                 .creation = AttachCreation::FirstTabInPrimary };
    }

    [[nodiscard]] AttachPlan newWorkspace()
    {
        return { .scope = FollowScope::Listed, .follow = {}, .creation = AttachCreation::NewWorkspace };
    }

    /// The first client of a daemon takes what is there; a later one gets a workspace of its own. A first one
    /// that names a working directory also opens a tab in it, when what is there leaves it nothing to create.
    [[nodiscard]] AttachPlan launch(AttachContext const& context)
    {
        if (context.otherClients != 0)
            return newWorkspace();
        auto plan = takeEverything(FollowScope::Listed, context);
        if (plan.creation == AttachCreation::None && context.workingDirectory == WorkingDirectoryStated::Yes)
            plan.creation = AttachCreation::TabInFirstFollowed;
        return plan;
    }
} // namespace

AttachPlan planAttach(proto::AttachIntent const& intent, AttachContext const& context)
{
    switch (intent.kind)
    {
        case proto::AttachIntentKind::AllWorkspaces: return takeEverything(FollowScope::Every, context);
        case proto::AttachIntentKind::Launch: return launch(context);
        case proto::AttachIntentKind::NewWorkspace: return newWorkspace();
        case proto::AttachIntentKind::Workspaces:
            return { .scope = FollowScope::Listed,
                     .follow = existing(intent.workspaces, context),
                     .creation = AttachCreation::None };
        case proto::AttachIntentKind::Resume: {
            // Review focus 2: the workspaces are gone (a restarted daemon), so this is a fresh launch. A
            // workspace holding no tab is no survivor: it is a restarted daemon's own empty primary
            // workspace, whose id is the lost one's only because every run counts from 1.
            auto follow = std::ranges::to<std::vector>(
                workspacesWithTabs(context) | std::views::filter([&](uint64_t id) {
                    return std::ranges::contains(intent.workspaces, id);
                }));
            if (follow.empty())
                return launch(context);
            return { .scope = FollowScope::Listed,
                     .follow = std::move(follow),
                     .creation = AttachCreation::None };
        }
    }
    return newWorkspace();
}

} // namespace vthost
