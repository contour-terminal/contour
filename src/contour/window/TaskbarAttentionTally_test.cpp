// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TaskbarAttentionTally.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <ranges>
#include <utility>

using namespace contour::window;

TEST_CASE("taskbarAttention.tallyCountsBlockedTabsPerWindowAndInAll", "[taskbarattention]")
{
    auto const tabs = std::array {
        std::pair { uint64_t { 1 }, TabBadgeKind::Blocked },
        std::pair { uint64_t { 1 }, TabBadgeKind::Done },
        std::pair { uint64_t { 2 }, TabBadgeKind::Blocked },
        std::pair { uint64_t { 2 }, TabBadgeKind::Blocked },
    };
    auto const tally = tallyWaitingTabs(tabs);
    CHECK(tally.total == 3);
    CHECK(tally.perWindow == std::vector<std::pair<uint64_t, unsigned>> { { 1, 1 }, { 2, 2 } });
}

TEST_CASE("taskbarAttention.aWindowWithNoneWaitingIsListedWithZero", "[taskbarattention]")
{
    auto const tabs = std::array {
        std::pair { uint64_t { 7 }, TabBadgeKind::Bell },
        std::pair { uint64_t { 7 }, TabBadgeKind::None },
        std::pair { uint64_t { 8 }, TabBadgeKind::Blocked },
    };
    auto const tally = tallyWaitingTabs(tabs);
    CHECK(tally.total == 1);
    CHECK(tally.perWindow == std::vector<std::pair<uint64_t, unsigned>> { { 7, 0 }, { 8, 1 } });
}

TEST_CASE("taskbarAttention.onlyBlockedWaits", "[taskbarattention]")
{
    for (auto const index: std::views::iota(size_t { 0 }, TabBadgeKindCount))
    {
        auto const kind = TabBadgeKind(index);
        auto const tabs = std::array { std::pair { uint64_t { 1 }, kind } };
        INFO("kind " << index);
        CHECK(tallyWaitingTabs(tabs).total == (kind == TabBadgeKind::Blocked ? 1u : 0u));
    }
}

TEST_CASE("taskbarAttention.noTabsNoTally", "[taskbarattention]")
{
    CHECK(tallyWaitingTabs({}) == AttentionTally {});
}
