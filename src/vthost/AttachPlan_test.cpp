// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <vthost/AttachPlan.hpp>

using namespace vthost;
using vthost::proto::AttachIntentKind;

namespace
{

/// @return A daemon with workspaces 1 (two tabs) and 3 (one tab), and no other client.
AttachContext twoWorkspaces()
{
    return { .otherClients = 0, .primaryWorkspace = 1, .workspaces = { { 1, 2 }, { 3, 1 } } };
}

using Ids = std::vector<uint64_t>;

} // namespace

TEST_CASE("AllWorkspaces follows every Left workspace with tabs", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::AllWorkspaces }, twoWorkspaces())
          == AttachPlan { .follow = { 1, 3 }, .creation = AttachCreation::None });
}

TEST_CASE("AllWorkspaces skips a workspace without tabs", "[vthost][attach-plan]")
{
    auto const context = AttachContext { .primaryWorkspace = 1, .workspaces = { { 1, 0 }, { 3, 1 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::AllWorkspaces }, context).follow == Ids { 3 });
}

TEST_CASE("AllWorkspaces against an empty daemon creates the first tab, as it always did",
          "[vthost][attach-plan]")
{
    auto const empty = AttachContext { .primaryWorkspace = 1, .workspaces = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::AllWorkspaces }, empty)
          == AttachPlan { .follow = { 1 }, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("an attach to an empty daemon creates the first tab", "[vthost][attach-plan]")
{
    auto const empty = AttachContext { .otherClients = 0, .primaryWorkspace = 1, .workspaces = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, empty)
          == AttachPlan { .follow = { 1 }, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("a Launch takes the Left workspaces and leaves the shown and hidden ones alone",
          "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, twoWorkspaces())
          == AttachPlan { .follow = { 1, 3 }, .creation = AttachCreation::None });

    auto const mixed = AttachContext { .primaryWorkspace = 1,
                                       .workspaces = { { 1, 1, WorkspaceVisibility::Left },
                                                       { 2, 1, WorkspaceVisibility::Hidden },
                                                       { 3, 1, WorkspaceVisibility::Left } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, mixed).follow == Ids { 1, 3 });
    CHECK(planAttach({ .kind = AttachIntentKind::AllWorkspaces }, mixed).follow == Ids { 1, 3 });
}

TEST_CASE("a Launch follows the Left workspaces whatever other clients are attached", "[vthost][attach-plan]")
{
    auto const context = AttachContext { .otherClients = 1,
                                         .primaryWorkspace = 1,
                                         .workspaces = { { 1, 1, WorkspaceVisibility::Left },
                                                         { 2, 1, WorkspaceVisibility::Hidden },
                                                         { 3, 1, WorkspaceVisibility::Left },
                                                         { 4, 1, WorkspaceVisibility::Shown } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, context)
          == AttachPlan { .follow = { 1, 3 }, .creation = AttachCreation::None });
}

TEST_CASE("a Launch with nothing Left creates a workspace of its own", "[vthost][attach-plan]")
{
    auto const hidden = AttachContext { .primaryWorkspace = 1,
                                        .workspaces = { { 1, 1, WorkspaceVisibility::Hidden },
                                                        { 2, 2, WorkspaceVisibility::Hidden } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, hidden)
          == AttachPlan { .follow = {}, .creation = AttachCreation::NewWorkspace });

    auto const shown = AttachContext { .otherClients = 1,
                                       .primaryWorkspace = 1,
                                       .workspaces = { { 1, 1, WorkspaceVisibility::Shown } } };
    CHECK(planAttach({ .kind = AttachIntentKind::AllWorkspaces }, shown)
          == AttachPlan { .follow = {}, .creation = AttachCreation::NewWorkspace });
}

TEST_CASE("NewWorkspace mirrors nothing", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::NewWorkspace, .workspaces = { 1 } }, twoWorkspaces())
          == AttachPlan { .follow = {}, .creation = AttachCreation::NewWorkspace });
}

TEST_CASE("Workspaces follows only the named workspaces that exist", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Workspaces, .workspaces = { 3, 99 } }, twoWorkspaces())
          == AttachPlan { .follow = { 3 }, .creation = AttachCreation::None });
    CHECK(planAttach({ .kind = AttachIntentKind::Workspaces, .workspaces = { 99 } }, twoWorkspaces())
              .follow.empty());
}

TEST_CASE("Workspaces lists ascending whatever order was named", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Workspaces, .workspaces = { 3, 1 } }, twoWorkspaces()).follow
          == Ids { 1, 3 });
}

TEST_CASE("Resume follows the survivors", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspaces = { 3, 99 } }, twoWorkspaces())
          == AttachPlan { .follow = { 3 }, .creation = AttachCreation::None });
}

TEST_CASE("Resume does not take an empty workspace for a survivor", "[vthost][attach-plan]")
{
    // A restarted daemon: its own primary workspace 1, which holds nothing yet, is not the workspace 1 the
    // client lost. Following it would show the client nothing, and spawn nothing.
    auto const fresh = AttachContext { .otherClients = 0, .primaryWorkspace = 1, .workspaces = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspaces = { 1 } }, fresh)
          == AttachPlan { .follow = { 1 }, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("Resume with no surviving workspace falls back to Launch", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspaces = { 99 } }, twoWorkspaces()).follow
          == Ids { 1, 3 });
    auto busy = twoWorkspaces();
    busy.otherClients = 2;
    for (auto& workspace: busy.workspaces)
        workspace.visibility = WorkspaceVisibility::Shown;
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspaces = { 99 } }, busy).creation
          == AttachCreation::NewWorkspace);
}

TEST_CASE("a Launch naming a directory opens a tab in the first workspace it brings back",
          "[vthost][attach-plan][cwd]")
{
    auto stated = twoWorkspaces();
    stated.workingDirectory = WorkingDirectoryStated::Yes;
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, stated)
          == AttachPlan { .follow = { 1, 3 }, .creation = AttachCreation::TabInFirstFollowed });
}

TEST_CASE("a Launch naming a directory creates what it would without one, when it creates anything",
          "[vthost][attach-plan][cwd]")
{
    // An empty daemon's first tab, and the own workspace of a launch that finds nothing Left, start in the
    // directory themselves.
    auto empty = AttachContext { .otherClients = 0, .primaryWorkspace = 1, .workspaces = { { 1, 0 } } };
    empty.workingDirectory = WorkingDirectoryStated::Yes;
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, empty).creation
          == AttachCreation::FirstTabInPrimary);
    auto busy = twoWorkspaces();
    busy.otherClients = 1;
    for (auto& workspace: busy.workspaces)
        workspace.visibility = WorkspaceVisibility::Shown;
    busy.workingDirectory = WorkingDirectoryStated::Yes;
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, busy).creation == AttachCreation::NewWorkspace);
}

TEST_CASE("only a Launch opens a tab for a stated directory", "[vthost][attach-plan][cwd]")
{
    // Following every workspace, named workspaces, or the survivors of a reconnect never adds a tab to them.
    auto stated = twoWorkspaces();
    stated.workingDirectory = WorkingDirectoryStated::Yes;
    CHECK(planAttach({ .kind = AttachIntentKind::AllWorkspaces }, stated).creation == AttachCreation::None);
    CHECK(planAttach({ .kind = AttachIntentKind::Workspaces, .workspaces = { 3 } }, stated).creation
          == AttachCreation::None);
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspaces = { 3 } }, stated).creation
          == AttachCreation::None);
}

TEST_CASE("ids of another run of the daemon name none of its workspaces", "[vthost][attach-plan]")
{
    auto const context = AttachContext { .primaryWorkspace = 1,
                                         .workspaces = { { 1, 1, WorkspaceVisibility::Left },
                                                         { 2, 1, WorkspaceVisibility::Hidden } },
                                         .instance = 8 };
    auto const ofRun = [](AttachIntentKind kind, uint64_t instance) {
        return vthost::proto::AttachIntent { .kind = kind, .workspaces = { 2 }, .daemonInstance = instance };
    };
    // This run's, or no run stated: the ids are this run's own.
    CHECK(planAttach(ofRun(AttachIntentKind::Workspaces, 8), context).follow == Ids { 2 });
    CHECK(planAttach(ofRun(AttachIntentKind::Workspaces, 0), context).follow == Ids { 2 });
    CHECK(planAttach(ofRun(AttachIntentKind::Resume, 8), context).follow == Ids { 2 });
    // Another run's: Workspaces follows nothing, and Resume is a launch, which leaves the hidden one alone.
    CHECK(planAttach(ofRun(AttachIntentKind::Workspaces, 7), context)
          == AttachPlan { .follow = {}, .creation = AttachCreation::None });
    CHECK(planAttach(ofRun(AttachIntentKind::Resume, 7), context)
          == AttachPlan { .follow = { 1 }, .creation = AttachCreation::None });
    // A daemon that states no run of its own takes the ids as they are.
    auto unstated = context;
    unstated.instance = 0;
    CHECK(planAttach(ofRun(AttachIntentKind::Workspaces, 7), unstated).follow == Ids { 2 });
}
