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

TEST_CASE("AllWorkspaces follows every workspace and future ones", "[vthost][attach-plan]")
{
    CHECK(
        planAttach({ .kind = AttachIntentKind::AllWorkspaces }, twoWorkspaces())
        == AttachPlan { .scope = FollowScope::Every, .follow = { 1, 3 }, .creation = AttachCreation::None });
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
          == AttachPlan {
              .scope = FollowScope::Every, .follow = { 1 }, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("an attach to an empty daemon creates the first tab", "[vthost][attach-plan]")
{
    auto const empty = AttachContext { .otherClients = 0, .primaryWorkspace = 1, .workspaces = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, empty)
          == AttachPlan {
              .scope = FollowScope::Listed, .follow = { 1 }, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("the first Launch takes every workspace, a later one a new workspace", "[vthost][attach-plan]")
{
    CHECK(
        planAttach({ .kind = AttachIntentKind::Launch }, twoWorkspaces())
        == AttachPlan { .scope = FollowScope::Listed, .follow = { 1, 3 }, .creation = AttachCreation::None });
    auto busy = twoWorkspaces();
    busy.otherClients = 1;
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, busy)
          == AttachPlan {
              .scope = FollowScope::Listed, .follow = {}, .creation = AttachCreation::NewWorkspace });
}

TEST_CASE("NewWorkspace mirrors nothing", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::NewWorkspace, .workspaces = { 1 } }, twoWorkspaces())
          == AttachPlan {
              .scope = FollowScope::Listed, .follow = {}, .creation = AttachCreation::NewWorkspace });
}

TEST_CASE("Workspaces follows only the named workspaces that exist", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Workspaces, .workspaces = { 3, 99 } }, twoWorkspaces())
          == AttachPlan { .scope = FollowScope::Listed, .follow = { 3 }, .creation = AttachCreation::None });
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
          == AttachPlan { .scope = FollowScope::Listed, .follow = { 3 }, .creation = AttachCreation::None });
}

TEST_CASE("Resume does not take an empty workspace for a survivor", "[vthost][attach-plan]")
{
    // A restarted daemon: its own primary workspace 1, which holds nothing yet, is not the workspace 1 the
    // client lost. Following it would show the client nothing, and spawn nothing.
    auto const fresh = AttachContext { .otherClients = 0, .primaryWorkspace = 1, .workspaces = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspaces = { 1 } }, fresh)
          == AttachPlan {
              .scope = FollowScope::Listed, .follow = { 1 }, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("Resume with no surviving workspace falls back to Launch", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspaces = { 99 } }, twoWorkspaces()).follow
          == Ids { 1, 3 });
    auto busy = twoWorkspaces();
    busy.otherClients = 2;
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .workspaces = { 99 } }, busy).creation
          == AttachCreation::NewWorkspace);
}
