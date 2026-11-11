// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

#include <vthost/AttachPlan.hpp>

using namespace vthost;
using vthost::proto::AttachIntentKind;

namespace
{

/// @return A daemon with workspaces 1 (two tabs) and 3 (one tab).
AttachContext twoWorkspaces()
{
    return { .primaryWorkspace = 1, .workspaces = { { 1, 2 }, { 3, 1 } } };
}

using Select = std::optional<uint64_t>;

} // namespace

TEST_CASE("an attach to an empty daemon creates the first tab", "[vthost][attach-plan]")
{
    auto const empty = AttachContext { .primaryWorkspace = 1, .workspaces = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Default }, empty)
          == AttachPlan { .select = 1, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("a Default launch selects the first workspace with a tab", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Default }, twoWorkspaces())
          == AttachPlan { .select = 1, .creation = AttachCreation::None });

    auto const firstEmpty = AttachContext { .primaryWorkspace = 1, .workspaces = { { 1, 0 }, { 2, 1 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Default }, firstEmpty).select == Select { 2 });
}

TEST_CASE("a Default launch selects the workspace selected last while it is there", "[vthost][attach-plan]")
{
    auto context = twoWorkspaces();
    context.lastSelected = 3;
    CHECK(planAttach({ .kind = AttachIntentKind::Default }, context).select == Select { 3 });
    context.lastSelected = 99;
    CHECK(planAttach({ .kind = AttachIntentKind::Default }, context).select == Select { 1 });
}

TEST_CASE("NewWorkspace selects what it creates", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::NewWorkspace, .workspace = 1 }, twoWorkspaces())
          == AttachPlan { .select = std::nullopt, .creation = AttachCreation::NewWorkspace });
}

TEST_CASE("Select selects the named workspace when it exists, else what a launch would",
          "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Select, .workspace = 3 }, twoWorkspaces())
          == AttachPlan { .select = 3, .creation = AttachCreation::None });
    CHECK(planAttach({ .kind = AttachIntentKind::Select, .workspace = 99 }, twoWorkspaces()).select
          == Select { 1 });
}

TEST_CASE("Resume selects the survivor", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspace = 3 }, twoWorkspaces())
          == AttachPlan { .select = 3, .creation = AttachCreation::None });
}

TEST_CASE("Resume does not take an empty workspace for a survivor", "[vthost][attach-plan]")
{
    // A restarted daemon: its own primary workspace 1, which holds nothing yet, is not the workspace 1 the
    // client lost. Selecting it would show the client nothing, and spawn nothing.
    auto const fresh = AttachContext { .primaryWorkspace = 1, .workspaces = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspace = 1 }, fresh)
          == AttachPlan { .select = 1, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("Resume with no surviving workspace falls back to a Default launch", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspace = 99 }, twoWorkspaces()).select
          == Select { 1 });
}

TEST_CASE("a Default launch naming a directory opens a tab in the workspace it selects",
          "[vthost][attach-plan][cwd]")
{
    auto stated = twoWorkspaces();
    stated.workingDirectory = WorkingDirectoryStated::Yes;
    CHECK(planAttach({ .kind = AttachIntentKind::Default }, stated)
          == AttachPlan { .select = 1, .creation = AttachCreation::TabInSelected });
}

TEST_CASE("a Default launch naming a directory on an empty daemon creates the first tab in it",
          "[vthost][attach-plan][cwd]")
{
    // The first tab starts in the directory itself.
    auto empty = AttachContext { .primaryWorkspace = 1, .workspaces = { { 1, 0 } } };
    empty.workingDirectory = WorkingDirectoryStated::Yes;
    CHECK(planAttach({ .kind = AttachIntentKind::Default }, empty).creation
          == AttachCreation::FirstTabInPrimary);
}

TEST_CASE("only a Default launch opens a tab for a stated directory", "[vthost][attach-plan][cwd]")
{
    // A named workspace, or the survivors of a reconnect never gets a tab added.
    auto stated = twoWorkspaces();
    stated.workingDirectory = WorkingDirectoryStated::Yes;
    CHECK(planAttach({ .kind = AttachIntentKind::Select, .workspace = 3 }, stated).creation
          == AttachCreation::None);
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspace = 3 }, stated).creation
          == AttachCreation::None);
}

TEST_CASE("ids of another run of the daemon name none of its workspaces", "[vthost][attach-plan]")
{
    auto context =
        AttachContext { .primaryWorkspace = 1, .workspaces = { { 1, 1 }, { 2, 1 } }, .instance = 8 };
    context.lastSelected = 1;
    auto const ofRun = [](AttachIntentKind kind, uint64_t instance) {
        return vthost::proto::AttachIntent { .kind = kind, .workspace = 2, .daemonInstance = instance };
    };
    // This run's, or no run stated: the ids are this run's own.
    CHECK(planAttach(ofRun(AttachIntentKind::Select, 8), context).select == Select { 2 });
    CHECK(planAttach(ofRun(AttachIntentKind::Select, 0), context).select == Select { 2 });
    CHECK(planAttach(ofRun(AttachIntentKind::Resume, 8), context).select == Select { 2 });
    // Another run's: a launch, which takes the workspace selected last.
    CHECK(planAttach(ofRun(AttachIntentKind::Select, 7), context).select == Select { 1 });
    CHECK(planAttach(ofRun(AttachIntentKind::Resume, 7), context).select == Select { 1 });
    // A daemon that states no run of its own takes the ids as they are.
    auto unstated = context;
    unstated.instance = 0;
    CHECK(planAttach(ofRun(AttachIntentKind::Select, 7), unstated).select == Select { 2 });
}
