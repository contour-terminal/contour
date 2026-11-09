// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>

#include <vthost/WorkspaceVisibility.hpp>

using namespace vthost;
using proto::WorkspaceRelease;

TEST_CASE("visibilityOf names a workspace Shown, Hidden or Left", "[vthost][visibility]")
{
    struct Row
    {
        std::size_t followers = 0;
        std::optional<WorkspaceRelease> lastRelease = std::nullopt;
        WorkspaceVisibility expected = WorkspaceVisibility::Left;
    };
    constexpr auto Rows = std::array {
        Row { 0, std::nullopt, WorkspaceVisibility::Left },
        Row { 0, WorkspaceRelease::Closed, WorkspaceVisibility::Left },
        Row { 0, WorkspaceRelease::SwitchedAway, WorkspaceVisibility::Hidden },
        Row { 1, std::nullopt, WorkspaceVisibility::Shown },
        Row { 2, WorkspaceRelease::SwitchedAway, WorkspaceVisibility::Shown },
        Row { 2, WorkspaceRelease::Closed, WorkspaceVisibility::Shown },
    };
    for (auto const& row: Rows)
        CHECK(visibilityOf(row.followers, row.lastRelease) == row.expected);
}

TEST_CASE("the visibility enumerators keep zero for Left", "[vthost][visibility]")
{
    STATIC_CHECK(WorkspaceVisibility {} == WorkspaceVisibility::Left);
}
