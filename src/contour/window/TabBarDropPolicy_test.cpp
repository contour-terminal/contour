// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TabBarDropPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

using namespace contour::window;

namespace
{
// Three tabs of 100 px with no spacing: boundaries at 0, 100, 200, 300.
constexpr auto Tabs = std::array { TabSpan { .left = 0, .right = 100 },
                                   TabSpan { .left = 100, .right = 200 },
                                   TabSpan { .left = 200, .right = 300 } };

constexpr TabBarHit hit(double x)
{
    return resolveTabBarHit(Tabs, x);
}
} // namespace

TEST_CASE("tabBar.dwellIsSixHundredMilliseconds", "[tabbar]")
{
    CHECK(TabDwell == std::chrono::milliseconds { 600 });
}

TEST_CASE("tabBar.boundaryHitsAreGaps", "[tabbar]")
{
    CHECK(hit(0).spot == TabBarSpot::Gap);
    CHECK(hit(0).index == 0);
    CHECK(hit(5).spot == TabBarSpot::Gap);
    CHECK(hit(5).index == 0);
    CHECK(hit(95).spot == TabBarSpot::Gap);
    CHECK(hit(95).index == 1);
    CHECK(hit(105).spot == TabBarSpot::Gap);
    CHECK(hit(105).index == 1);
    CHECK(hit(205).index == 2);
    CHECK(hit(295).spot == TabBarSpot::Gap);
    CHECK(hit(295).index == 3);
    CHECK(hit(300).index == 3);
    CHECK(hit(305).index == 3);
}

TEST_CASE("tabBar.midTabIsOnTab", "[tabbar]")
{
    CHECK(hit(50).spot == TabBarSpot::OnTab);
    CHECK(hit(50).index == 0);
    CHECK(hit(150).spot == TabBarSpot::OnTab);
    CHECK(hit(150).index == 1);
    CHECK(hit(250).spot == TabBarSpot::OnTab);
    CHECK(hit(250).index == 2);
    CHECK(hit(11).spot == TabBarSpot::OnTab);
    CHECK(hit(89).spot == TabBarSpot::OnTab);
}

TEST_CASE("tabBar.farRightIsTheEndGap", "[tabbar]")
{
    CHECK(hit(500).spot == TabBarSpot::Gap);
    CHECK(hit(500).index == 3);
}

TEST_CASE("tabBar.farLeftIsTheFirstGap", "[tabbar]")
{
    CHECK(hit(-50).spot == TabBarSpot::Gap);
    CHECK(hit(-50).index == 0);
}

TEST_CASE("tabBar.aNarrowTabKeepsAMiddle", "[tabbar]")
{
    // Two tabs of 15 px: a 10 px half-gap would leave neither a middle, so each gap is a quarter tab wide.
    constexpr auto Narrow =
        std::array { TabSpan { .left = 0, .right = 15 }, TabSpan { .left = 15, .right = 30 } };
    CHECK(tabGapHalfWidthOf(Narrow[0]) == 3.75);
    CHECK(tabGapHalfWidthOf(Tabs[0]) == TabGapHalfWidth);
    auto const at = [&](double x) {
        return resolveTabBarHit(Narrow, x);
    };
    CHECK(at(2).spot == TabBarSpot::Gap);
    CHECK(at(2).index == 0);
    CHECK(at(7).spot == TabBarSpot::OnTab);
    CHECK(at(7).index == 0);
    CHECK(at(13).spot == TabBarSpot::Gap);
    CHECK(at(13).index == 1);
    CHECK(at(22).spot == TabBarSpot::OnTab);
    CHECK(at(22).index == 1);
    CHECK(at(29).spot == TabBarSpot::Gap);
    CHECK(at(29).index == 2);
}

TEST_CASE("tabBar.emptyStripIsGapZero", "[tabbar]")
{
    auto const result = resolveTabBarHit({}, 42.0);
    CHECK(result.spot == TabBarSpot::Gap);
    CHECK(result.index == 0);
}

TEST_CASE("tabBar.shouldSpringOpenTable", "[tabbar]")
{
    for (auto const subject: { DwellSubject::OtherTab, DwellSubject::DraggedTab, DwellSubject::SourceTab })
        for (auto const refusal:
             { MoveRefusal::None, MoveRefusal::OtherProcess, MoveRefusal::OldDaemon, MoveRefusal::Tmux })
        {
            auto const expected = subject == DwellSubject::OtherTab && refusal == MoveRefusal::None
                                      ? DwellOpens::Yes
                                      : DwellOpens::No;
            CHECK(shouldSpringOpen(subject, refusal) == expected);
        }
}
