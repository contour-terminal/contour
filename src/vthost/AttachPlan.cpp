// SPDX-License-Identifier: Apache-2.0
#include <vthost/AttachPlan.hpp>

#include <algorithm>
#include <ranges>

#include <vthost/ViewRouting.hpp>

namespace vthost
{

namespace
{
    /// @return The ids of the workspaces holding at least one tab, ascending.
    [[nodiscard]] std::vector<uint64_t> workspacesWithTabs(AttachContext const& context)
    {
        return std::ranges::to<std::vector>(
            context.workspaces
            | std::views::filter([](WorkspaceSummary const& workspace) { return workspace.tabCount > 0; })
            | std::views::transform(&WorkspaceSummary::id));
    }

    /// @return Whether the intent's workspace id is of this run of the daemon: a restarted daemon's ids count
    ///         from 1 again, so one of another run would name another workspace than the one the client
    ///         showed.
    [[nodiscard]] bool namesThisRun(proto::AttachIntent const& intent, AttachContext const& context)
    {
        return intent.daemonInstance == 0 || context.instance == 0
               || intent.daemonInstance == context.instance;
    }

    /// What a launch selects: the workspace selected last, else the first one with a tab. A daemon with no
    /// tab at all gets its first one in the primary workspace, which is then the selection.
    [[nodiscard]] AttachPlan launch(AttachContext const& context)
    {
        auto const ids = workspacesWithTabs(context);
        auto const selection = defaultSelection(ids, context.lastSelected);
        if (!selection)
            return { .select = context.primaryWorkspace, .creation = AttachCreation::FirstTabInPrimary };
        // A launch that names a working directory also opens a tab in it: the workspaces it brings back
        // leave it nothing to create.
        return { .select = selection,
                 .creation = context.workingDirectory == WorkingDirectoryStated::Yes
                                 ? AttachCreation::TabInSelected
                                 : AttachCreation::None };
    }
} // namespace

AttachPlan planAttach(proto::AttachIntent const& intent, AttachContext const& context)
{
    switch (intent.kind)
    {
        case proto::AttachIntentKind::Default: return launch(context);
        case proto::AttachIntentKind::NewWorkspace:
            return { .select = std::nullopt, .creation = AttachCreation::NewWorkspace };
        case proto::AttachIntentKind::Select: {
            // A connection selects something: a workspace that is not there is a launch's choice.
            auto const named =
                std::ranges::any_of(context.workspaces, [&](WorkspaceSummary const& workspace) {
                    return workspace.id == intent.workspace;
                });
            if (named && namesThisRun(intent, context))
                return { .select = intent.workspace, .creation = AttachCreation::None };
            return launch(context);
        }
        case proto::AttachIntentKind::Resume: {
            // The workspace is gone (a restarted daemon), so this is a fresh launch. A workspace holding no
            // tab is no survivor: it is a restarted daemon's own empty primary workspace, whose id is the
            // lost one's only because every run counts from 1.
            if (namesThisRun(intent, context)
                && std::ranges::contains(workspacesWithTabs(context), intent.workspace))
                return { .select = intent.workspace, .creation = AttachCreation::None };
            return launch(context);
        }
    }
    return { .select = std::nullopt, .creation = AttachCreation::NewWorkspace };
}

} // namespace vthost
