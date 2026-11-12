// SPDX-License-Identifier: Apache-2.0
#include <vthost/AttachPlan.hpp>

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <utility>

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

    /// @return The ids @p intent names, unless they are of another run of the daemon than this one: a
    ///         restarted daemon's ids count from 1 again, so they would name other workspaces than the ones
    ///         the client showed.
    [[nodiscard]] std::vector<uint64_t> namedInThisRun(proto::AttachIntent const& intent,
                                                       AttachContext const& context)
    {
        if (intent.daemonInstance != 0 && context.instance != 0 && intent.daemonInstance != context.instance)
            return {};
        return intent.workspaces;
    }

    [[nodiscard]] AttachPlan newWorkspace()
    {
        return { .follow = {}, .creation = AttachCreation::NewWorkspace };
    }

    /// Which of the Left workspaces takeLeft() brings back.
    enum class LeftTaken : uint8_t
    {
        /// The one left last: a launch opens one window however many workspaces wait, the others being the
        /// next launches', or the switcher's.
        Latest = 0,
        /// Every one: what a client predating attach intents, which shows every workspace, follows.
        Every = 1,
    };

    /// What a launch brings back: Left workspaces with tabs, as @p taken says, wherever else clients are
    /// attached. A daemon with no tab at all gets its first one in the primary workspace; one whose
    /// workspaces are all shown or hidden gets a workspace of its own.
    [[nodiscard]] AttachPlan takeLeft(AttachContext const& context, LeftTaken taken)
    {
        auto left = context.workspaces | std::views::filter([](WorkspaceSummary const& workspace) {
                        return workspace.tabCount > 0 && workspace.visibility == WorkspaceVisibility::Left;
                    });
        if (!std::ranges::empty(left))
        {
            if (taken == LeftTaken::Every)
                return { .follow = std::ranges::to<std::vector>(
                             left | std::views::transform(&WorkspaceSummary::id)),
                         .creation = AttachCreation::None };
            auto const latest = std::ranges::max(left, {}, [](WorkspaceSummary const& workspace) {
                return std::pair { workspace.leftAt, workspace.id };
            });
            return { .follow = { latest.id }, .creation = AttachCreation::None };
        }
        if (workspacesWithTabs(context).empty())
            return { .follow = { context.primaryWorkspace }, .creation = AttachCreation::FirstTabInPrimary };
        return newWorkspace();
    }

    /// What takeLeft() brings back; a launch that names a working directory also opens a tab in it, when what
    /// is brought back leaves it nothing to create.
    [[nodiscard]] AttachPlan launch(AttachContext const& context)
    {
        auto plan = takeLeft(context, LeftTaken::Latest);
        if (plan.creation == AttachCreation::None && context.workingDirectory == WorkingDirectoryStated::Yes)
            plan.creation = AttachCreation::TabInFirstFollowed;
        return plan;
    }
} // namespace

AttachPlan planAttach(proto::AttachIntent const& intent, AttachContext const& context)
{
    switch (intent.kind)
    {
        case proto::AttachIntentKind::AllWorkspaces: return takeLeft(context, LeftTaken::Every);
        case proto::AttachIntentKind::Launch: return launch(context);
        case proto::AttachIntentKind::NewWorkspace: return newWorkspace();
        case proto::AttachIntentKind::Workspaces:
            return { .follow = existing(namedInThisRun(intent, context), context),
                     .creation = AttachCreation::None };
        case proto::AttachIntentKind::Resume: {
            // Review focus 2: the workspaces are gone (a restarted daemon), so this is a fresh launch. A
            // workspace holding no tab is no survivor: it is a restarted daemon's own empty primary
            // workspace, whose id is the lost one's only because every run counts from 1.
            auto const named = namedInThisRun(intent, context);
            auto follow = std::ranges::to<std::vector>(
                workspacesWithTabs(context)
                | std::views::filter([&](uint64_t id) { return std::ranges::contains(named, id); }));
            if (follow.empty())
            {
                // Its workspaces are gone with a restart: the windows that run showed take back what they
                // showed, all of them, where a launch would take one.
                auto restored = std::ranges::to<std::vector>(
                    context.workspaces | std::views::filter([](WorkspaceSummary const& workspace) {
                        return workspace.tabCount > 0 && workspace.visibility == WorkspaceVisibility::Left
                               && workspace.shownBeforeRestart == ShownBeforeRestart::Yes;
                    })
                    | std::views::transform(&WorkspaceSummary::id));
                if (!restored.empty())
                    return { .follow = std::move(restored), .creation = AttachCreation::None };
                return launch(context);
            }
            return { .follow = std::move(follow), .creation = AttachCreation::None };
        }
    }
    return newWorkspace();
}

} // namespace vthost
