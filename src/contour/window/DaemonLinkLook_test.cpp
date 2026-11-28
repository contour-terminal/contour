// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DaemonLinkLook.hpp>
#include <contour/window/GraphemeElide.hpp>
#include <contour/window/TabBadge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>

using namespace contour::window;
using contour::config::SidebarLook;

namespace
{

constexpr auto SidebarLooks = std::array {
    SidebarLook::SourceList, SidebarLook::NavigationPane, SidebarLook::HeaderPane, SidebarLook::CellTree
};
static_assert(SidebarLooks.size() == static_cast<size_t>(SidebarLook::CellTree) + 1,
              "one entry per sidebar look");

} // namespace

TEST_CASE("link-state glyphs never equal a tab-status glyph, per look", "[daemon-link]")
{
    for (auto const look: SidebarLooks)
    {
        for (auto const s: std::views::iota(size_t { 0 }, DaemonLinkStateCount))
        {
            auto const state = DaemonLinkState(s);
            auto const glyph = linkLookOf(state, look).glyph;
            INFO("look " << static_cast<int>(look) << ", state " << s);
            CHECK_FALSE(glyph.empty());
            for (auto const k: std::views::iota(size_t { 1 }, TabBadgeKindCount))
                CHECK(glyph != tabBadgeGlyph(TabBadgeKind(k), look));
            for (auto const other: std::views::iota(s + 1, DaemonLinkStateCount))
                CHECK(glyph != linkLookOf(DaemonLinkState(other), look).glyph);
        }
    }
}

TEST_CASE("link-state glyphs are one cell on the Terminal row", "[daemon-link]")
{
    for (auto const s: std::views::iota(size_t { 0 }, DaemonLinkStateCount))
    {
        auto const clusters = clustersOf(linkLookOf(DaemonLinkState(s), SidebarLook::CellTree).glyph);
        INFO("state " << s);
        REQUIRE(clusters.size() == 1);
        CHECK(clusters.front().cells == 1);
    }
}

TEST_CASE("every link state has words and accessible words", "[daemon-link]")
{
    for (auto const& look: DaemonLinkLooks)
    {
        INFO("state " << static_cast<int>(look.state));
        CHECK_FALSE(look.words.empty());
        CHECK_FALSE(look.accessibleWords.empty());
    }
    CHECK(DaemonLinkLooks[static_cast<size_t>(DaemonLinkState::NeedsInput)].words == "Needs you");
    CHECK(DaemonLinkLooks[static_cast<size_t>(DaemonLinkState::TooOld)].words == "Daemon too old");
}

TEST_CASE("link looks are listed in enumerator order", "[daemon-link]")
{
    REQUIRE(DaemonLinkLooks.size() == DaemonLinkStateCount);
    for (auto const i: std::views::iota(size_t { 0 }, DaemonLinkLooks.size()))
        CHECK(DaemonLinkLooks[i].state == DaemonLinkState(i));
}

TEST_CASE("a live link state beats discovery's Not running", "[daemon-link]")
{
    CHECK(precedenceOf(DaemonLinkState::Reconnecting, DaemonLinkState::NotRunning)
          == DaemonLinkState::Reconnecting);
    CHECK(precedenceOf(DaemonLinkState::Connected, DaemonLinkState::NotRunning)
          == DaemonLinkState::Connected);
    CHECK(precedenceOf(DaemonLinkState::Connecting, DaemonLinkState::NotRunning)
          == DaemonLinkState::Connecting);
    CHECK(precedenceOf(DaemonLinkState::NotConnected, DaemonLinkState::NotRunning)
          == DaemonLinkState::NotRunning);
    CHECK(precedenceOf(DaemonLinkState::Failed, DaemonLinkState::NotConnected) == DaemonLinkState::Failed);
}
