// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ProgramStatusBadge.hpp>
#include <contour/window/StatusUrgency.hpp>
#include <contour/window/TabBadge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>

using namespace contour::window;

TEST_CASE("urgency rank is needs-you > failed > done-unseen > working > unseen-output > idle", "[urgency]")
{
    CHECK(Urgency::NeedsYou > Urgency::Failed);
    CHECK(Urgency::Failed > Urgency::DoneUnseen);
    CHECK(Urgency::DoneUnseen > Urgency::Working);
    CHECK(Urgency::Working > Urgency::UnseenOutput);
    CHECK(Urgency::UnseenOutput > Urgency::Idle);
    CHECK(Urgency::Idle == Urgency {});
}

TEST_CASE("bell maps to needs-you", "[urgency]")
{
    CHECK(urgencyOf(TabBadgeKind::Bell) == Urgency::NeedsYou);
    CHECK(urgencyOf(TabBadgeKind::Blocked) == Urgency::NeedsYou);
    CHECK(urgencyOf(TabBadgeKind::Error) == Urgency::Failed);
    CHECK(urgencyOf(TabBadgeKind::Done) == Urgency::DoneUnseen);
    CHECK(urgencyOf(TabBadgeKind::Working) == Urgency::Working);
    CHECK(urgencyOf(TabBadgeKind::Output) == Urgency::UnseenOutput);
    CHECK(urgencyOf(TabBadgeKind::None) == Urgency::Idle);
}

TEST_CASE("a seen done is idle, a seen error is idle", "[urgency]")
{
    CHECK(urgencyOf(BadgeKind::Done, StatusSeen::Unseen) == Urgency::DoneUnseen);
    CHECK(urgencyOf(BadgeKind::Done, StatusSeen::Seen) == Urgency::Idle);
    CHECK(urgencyOf(BadgeKind::Error, StatusSeen::Unseen) == Urgency::Failed);
    CHECK(urgencyOf(BadgeKind::Error, StatusSeen::Seen) == Urgency::Idle);
    CHECK(urgencyOf(BadgeKind::Blocked, StatusSeen::Seen) == Urgency::NeedsYou);
    CHECK(urgencyOf(BadgeKind::Working, StatusSeen::Seen) == Urgency::Working);
    CHECK(urgencyOf(BadgeKind::None, StatusSeen::Unseen) == Urgency::Idle);
}

TEST_CASE("roll-up of nothing is idle", "[urgency]")
{
    CHECK(rollUp({}) == Urgency::Idle);
    auto const some = std::array { Urgency::Working, Urgency::Failed, Urgency::UnseenOutput };
    CHECK(rollUp(some) == Urgency::Failed);
}

TEST_CASE("every TabBadgeKind maps", "[urgency]")
{
    static_assert(TabBadgeKindCount == 7, "a new TabBadgeKind needs a row in urgencyOf() and in this test");
    for (auto const index: std::views::iota(size_t { 1 }, TabBadgeKindCount))
        CHECK(urgencyOf(TabBadgeKind(index)) != Urgency::Idle);
}

TEST_CASE("an unseen done outranks working in the program status table", "[urgency][badge]")
{
    auto done = vtbackend::ProgramStatusRecord {};
    done.state = vtbackend::ProgramState::Done;
    done.generation = 2;
    auto working = vtbackend::ProgramStatusRecord {};
    working.state = vtbackend::ProgramState::Working;
    auto const records = std::array { working, done };
    CHECK(badgeFor(records, 0).kind == BadgeKind::Done);
    CHECK(badgeFor(records, 2).kind == BadgeKind::Working);
}
