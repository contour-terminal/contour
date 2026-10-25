// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TabBadge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <utility>

using namespace contour::window;
using contour::config::StatusColorRole;
using vtbackend::CommandEnding;

TEST_CASE("tabBadge.priority", "[tabbadge]")
{
    constexpr auto Failed = FinishedBlockBadge::Failed;
    constexpr auto Succeeded = FinishedBlockBadge::Succeeded;
    constexpr auto Unseen = TabActivity::UnseenOutput;

    CHECK(
        resolveTabBadge({ .programStatus = BadgeKind::Blocked, .finishedBlock = Failed, .activity = Unseen })
        == TabBadge { TabBadgeKind::Blocked });
    CHECK(resolveTabBadge({ BadgeKind::Working, 40, Failed, Unseen }) == TabBadge { TabBadgeKind::Error });
    CHECK(resolveTabBadge({ BadgeKind::Working, 40, FinishedBlockBadge::None, Unseen })
          == TabBadge { TabBadgeKind::Working, 40 });
    CHECK(resolveTabBadge({ BadgeKind::None, std::nullopt, Succeeded, Unseen })
          == TabBadge { TabBadgeKind::Done });
    CHECK(resolveTabBadge({ .programStatus = BadgeKind::Done }) == TabBadge { TabBadgeKind::Done });
    CHECK(resolveTabBadge({ .activity = Unseen }) == TabBadge { TabBadgeKind::Output });
    CHECK(resolveTabBadge({}) == TabBadge {});
}

TEST_CASE("tabBadge.everyAdjacentPairResolvesToTheHigher", "[tabbadge]")
{
    struct Pair
    {
        TabBadgeKind higher = TabBadgeKind::None; ///< The rule that must win.
        TabBadgeKind lower = TabBadgeKind::None;  ///< The rule right below it.
        TabBadgeInputs inputs;                    ///< An input both rules match.
    };
    constexpr auto Unseen = TabActivity::UnseenOutput;
    // One row per adjacent pair of TabBadgeRules, each input selecting both rules through different fields.
    auto const pairs = std::array {
        Pair { TabBadgeKind::Blocked,
               TabBadgeKind::Error,
               { .programStatus = BadgeKind::Blocked, .finishedBlock = FinishedBlockBadge::Failed } },
        Pair { TabBadgeKind::Error,
               TabBadgeKind::Working,
               { .programStatus = BadgeKind::Working, .finishedBlock = FinishedBlockBadge::Failed } },
        Pair { TabBadgeKind::Working,
               TabBadgeKind::Done,
               { .programStatus = BadgeKind::Working, .finishedBlock = FinishedBlockBadge::Succeeded } },
        Pair { TabBadgeKind::Done,
               TabBadgeKind::Output,
               { .finishedBlock = FinishedBlockBadge::Succeeded, .activity = Unseen } },
    };
    REQUIRE(pairs.size() + 1 == TabBadgeRules.size());
    for (auto const i: std::views::iota(size_t { 0 }, pairs.size()))
    {
        auto const& pair = pairs[i];
        CHECK(pair.higher == TabBadgeRules[i].kind);
        CHECK(pair.lower == TabBadgeRules[i + 1].kind);
        CHECK(tabBadgeRuleMatches(TabBadgeRules[i], pair.inputs));
        CHECK(tabBadgeRuleMatches(TabBadgeRules[i + 1], pair.inputs));
        CHECK(resolveTabBadge(pair.inputs).kind == pair.higher);
    }
    CHECK(resolveTabBadge({ BadgeKind::Working, 40, FinishedBlockBadge::Succeeded, TabActivity::None })
          == TabBadge { TabBadgeKind::Working, 40 });
    CHECK(resolveTabBadge({ BadgeKind::Done, std::nullopt, FinishedBlockBadge::None, Unseen })
          == TabBadge { TabBadgeKind::Done });
}

TEST_CASE("tabBadge.singleSources", "[tabbadge]")
{
    CHECK(resolveTabBadge({ .programStatus = BadgeKind::Error }) == TabBadge { TabBadgeKind::Error });
    CHECK(resolveTabBadge({ .finishedBlock = FinishedBlockBadge::Succeeded })
          == TabBadge { TabBadgeKind::Done });
    CHECK(resolveTabBadge({ .finishedBlock = FinishedBlockBadge::Failed })
          == TabBadge { TabBadgeKind::Error });
}

TEST_CASE("tabBadge.progressOnlyForWorking", "[tabbadge]")
{
    CHECK(resolveTabBadge({ BadgeKind::Done, 40 }).progressPercent == std::nullopt);
    CHECK(resolveTabBadge({ BadgeKind::Working, 40 }).progressPercent == 40);
}

TEST_CASE("tabBadge.everyKindHasALook", "[tabbadge]")
{
    for (auto const i: std::views::iota(size_t { 0 }, TabBadgeKindCount))
    {
        auto const kind = TabBadgeKind(i);
        auto const& look = tabBadgeLook(kind);
        CHECK(TabBadgeLooks[i].kind == kind);
        CHECK(look.kind == kind);
        if (kind != TabBadgeKind::None)
        {
            CHECK_FALSE(look.glyph.empty());
            CHECK_FALSE(look.accessibleName.empty());
        }
    }
    CHECK(tabBadgeLook(TabBadgeKind::Working).ink == StatusColorRole::Running);
    CHECK(tabBadgeLook(TabBadgeKind::Output).mark == BadgeMark::Dot);
    CHECK(tabBadgeLook(TabBadgeKind::Working).mark == BadgeMark::Ring);
    CHECK(tabBadgeLook(TabBadgeKind::Blocked).mark == BadgeMark::Disc);
    CHECK(tabBadgeLook(TabBadgeKind::Blocked).ink == StatusColorRole::Warning);
    CHECK(tabBadgeLook(TabBadgeKind::Error).ink == StatusColorRole::Danger);
    CHECK(tabBadgeLook(TabBadgeKind::Done).ink == StatusColorRole::Success);

    CHECK(tabBadgeLook(TabBadgeKind::Done).glyph == "✓");
    CHECK(tabBadgeLook(TabBadgeKind::Error).glyph == "✕");
    CHECK(tabBadgeLook(TabBadgeKind::Blocked).glyph == "!");
    CHECK(tabBadgeLook(TabBadgeKind::Done).accessibleName == "Done");
    CHECK(tabBadgeLook(TabBadgeKind::Error).accessibleName == "Failed");
    CHECK(tabBadgeLook(TabBadgeKind::Working).accessibleName == "Working");
    CHECK(tabBadgeLook(TabBadgeKind::Output).accessibleName == "New output");
    CHECK(tabBadgeLook(TabBadgeKind::Blocked).accessibleName == "Waiting for you");
}

TEST_CASE("tabBadge.badgeMarkName", "[tabbadge]")
{
    CHECK(badgeMarkName(BadgeMark::None) == "none");
    CHECK(badgeMarkName(BadgeMark::Dot) == "dot");
    CHECK(badgeMarkName(BadgeMark::Ring) == "ring");
    CHECK(badgeMarkName(BadgeMark::Disc) == "disc");
    CHECK(badgeMarkName(BadgeMark::Glyph) == "glyph");
}

TEST_CASE("tabBadge.rulesCoverEveryKind", "[tabbadge]")
{
    for (auto const i: std::views::iota(size_t { 1 }, TabBadgeKindCount))
    {
        auto const kind = TabBadgeKind(i);
        CHECK(std::ranges::count(TabBadgeRules, kind, &TabBadgeRule::kind) == 1);
    }
    CHECK(TabBadgeRules.front().kind == TabBadgeKind::Blocked);
}

TEST_CASE("tabBadge.finishedBlockOfEveryEnding", "[tabbadge]")
{
    struct Row
    {
        CommandEnding ending;
        FinishedBlockBadge badge;
    };
    // Failures follow vtbackend::isFailure, so a tab never disagrees with the gutter.
    constexpr auto Expected = std::array {
        Row { CommandEnding::Unknown, FinishedBlockBadge::None },
        Row { CommandEnding::Succeeded, FinishedBlockBadge::Succeeded },
        Row { CommandEnding::Failed, FinishedBlockBadge::Failed },
        Row { CommandEnding::Killed, FinishedBlockBadge::Failed },
        Row { CommandEnding::Crashed, FinishedBlockBadge::Failed },
        Row { CommandEnding::Interrupted, FinishedBlockBadge::Failed },
    };
    REQUIRE(Expected.size() == vtbackend::CommandEndingCount);
    for (auto const i: std::views::iota(size_t { 0 }, Expected.size()))
    {
        CHECK(Expected[i].ending == CommandEnding(i));
        CHECK(finishedBlockBadgeOf(Expected[i].ending) == Expected[i].badge);
        CHECK((Expected[i].badge == FinishedBlockBadge::Failed) == vtbackend::isFailure(Expected[i].ending));
    }
    CHECK(strongerFinishedBlock(FinishedBlockBadge::Succeeded, FinishedBlockBadge::Failed)
          == FinishedBlockBadge::Failed);
    CHECK(strongerFinishedBlock(FinishedBlockBadge::Failed, FinishedBlockBadge::Succeeded)
          == FinishedBlockBadge::Failed);
    CHECK(strongerFinishedBlock(FinishedBlockBadge::None, FinishedBlockBadge::Succeeded)
          == FinishedBlockBadge::Succeeded);
    CHECK(strongerFinishedBlock(FinishedBlockBadge::None, FinishedBlockBadge::None)
          == FinishedBlockBadge::None);
}
