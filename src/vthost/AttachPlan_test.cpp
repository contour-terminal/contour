// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <vthost/AttachPlan.hpp>

using namespace vthost;
using vthost::proto::AttachIntentKind;

namespace
{

/// @return A daemon with windows 1 (two tabs) and 3 (one tab), and no other client.
AttachContext twoWindows()
{
    return { .otherClients = 0, .primaryWindow = 1, .windows = { { 1, 2 }, { 3, 1 } } };
}

using Ids = std::vector<uint64_t>;

} // namespace

TEST_CASE("AllWindows follows every window and future ones", "[vthost][attach-plan]")
{
    CHECK(
        planAttach({ .kind = AttachIntentKind::AllWindows }, twoWindows())
        == AttachPlan { .scope = FollowScope::Every, .follow = { 1, 3 }, .creation = AttachCreation::None });
}

TEST_CASE("AllWindows skips a window without tabs", "[vthost][attach-plan]")
{
    auto const context = AttachContext { .primaryWindow = 1, .windows = { { 1, 0 }, { 3, 1 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::AllWindows }, context).follow == Ids { 3 });
}

TEST_CASE("AllWindows against an empty daemon creates the first tab, as it always did",
          "[vthost][attach-plan]")
{
    auto const empty = AttachContext { .primaryWindow = 1, .windows = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::AllWindows }, empty)
          == AttachPlan {
              .scope = FollowScope::Every, .follow = { 1 }, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("an attach to an empty daemon creates the first tab", "[vthost][attach-plan]")
{
    auto const empty = AttachContext { .otherClients = 0, .primaryWindow = 1, .windows = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Launch }, empty)
          == AttachPlan {
              .scope = FollowScope::Listed, .follow = { 1 }, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("the first Launch takes every window, a later one a new window", "[vthost][attach-plan]")
{
    CHECK(
        planAttach({ .kind = AttachIntentKind::Launch }, twoWindows())
        == AttachPlan { .scope = FollowScope::Listed, .follow = { 1, 3 }, .creation = AttachCreation::None });
    auto busy = twoWindows();
    busy.otherClients = 1;
    CHECK(
        planAttach({ .kind = AttachIntentKind::Launch }, busy)
        == AttachPlan { .scope = FollowScope::Listed, .follow = {}, .creation = AttachCreation::NewWindow });
}

TEST_CASE("NewWindow mirrors nothing", "[vthost][attach-plan]")
{
    CHECK(
        planAttach({ .kind = AttachIntentKind::NewWindow, .windows = { 1 } }, twoWindows())
        == AttachPlan { .scope = FollowScope::Listed, .follow = {}, .creation = AttachCreation::NewWindow });
}

TEST_CASE("Windows follows only the named windows that exist", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Windows, .windows = { 3, 99 } }, twoWindows())
          == AttachPlan { .scope = FollowScope::Listed, .follow = { 3 }, .creation = AttachCreation::None });
    CHECK(planAttach({ .kind = AttachIntentKind::Windows, .windows = { 99 } }, twoWindows()).follow.empty());
}

TEST_CASE("Windows lists ascending whatever order was named", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Windows, .windows = { 3, 1 } }, twoWindows()).follow
          == Ids { 1, 3 });
}

TEST_CASE("Resume follows the survivors", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .windows = { 3, 99 } }, twoWindows())
          == AttachPlan { .scope = FollowScope::Listed, .follow = { 3 }, .creation = AttachCreation::None });
}

TEST_CASE("Resume does not take an empty window for a survivor", "[vthost][attach-plan]")
{
    // A restarted daemon: its own primary window 1, which holds nothing yet, is not the window 1 the client
    // lost. Following it would show the client nothing, and spawn nothing.
    auto const fresh = AttachContext { .otherClients = 0, .primaryWindow = 1, .windows = { { 1, 0 } } };
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .windows = { 1 } }, fresh)
          == AttachPlan {
              .scope = FollowScope::Listed, .follow = { 1 }, .creation = AttachCreation::FirstTabInPrimary });
}

TEST_CASE("Resume with no surviving window falls back to Launch", "[vthost][attach-plan]")
{
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .windows = { 99 } }, twoWindows()).follow
          == Ids { 1, 3 });
    auto busy = twoWindows();
    busy.otherClients = 2;
    CHECK(planAttach({ .kind = AttachIntentKind::Resume, .windows = { 99 } }, busy).creation
          == AttachCreation::NewWindow);
}
