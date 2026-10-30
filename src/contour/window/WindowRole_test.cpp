// SPDX-License-Identifier: Apache-2.0
#include <contour/window/WindowRole.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>
#include <utility>

using namespace contour::window;

TEST_CASE("WindowRole.oneRowPerRoleInOrder", "[windowrole]")
{
    CHECK(WindowRoleRows.size() == WindowRoleCount);
    for (auto const index: std::views::iota(size_t { 0 }, WindowRoleRows.size()))
        CHECK(std::to_underlying(WindowRoleRows[index].role) == index);
}

TEST_CASE("WindowRole.theQuickTerminalIsAPlacedMinimalPanel", "[windowrole]")
{
    auto const& row = windowRoleRow(WindowRole::QuickTerminal);
    CHECK(row.level == WindowLevel::Panel);
    CHECK(row.density == ChromeDensityOverride::Minimal);
    CHECK(row.memory == PositionMemory::Placed);
}

TEST_CASE("WindowRole.aNormalWindowIsConfiguredAndRemembered", "[windowrole]")
{
    auto const& row = windowRoleRow(WindowRole::Normal);
    CHECK(row.level == WindowLevel::Normal);
    CHECK(row.density == ChromeDensityOverride::Configured);
    CHECK(row.memory == PositionMemory::Remembered);
}

TEST_CASE("WindowRole.theQuickTerminalIsMinimalWhateverIsConfigured", "[windowrole]")
{
    using contour::config::ChromeDensity;
    for (auto const configured: { ChromeDensity::Comfortable, ChromeDensity::Minimal })
    {
        CHECK(chromeDensityFor(WindowRole::QuickTerminal, configured) == ChromeDensity::Minimal);
        CHECK(chromeDensityFor(WindowRole::Normal, configured) == configured);
    }
}
