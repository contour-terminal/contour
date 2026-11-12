// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarMenuState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace contour::window;

namespace
{
[[nodiscard]] DirectoryWorkspace workspace(uint64_t id, std::string label = {})
{
    return DirectoryWorkspace { .id = id, .label = std::move(label), .tabs = {} };
}
} // namespace

TEST_CASE("move to workspace lists the other workspaces", "[sidebarmenu]")
{
    auto const directory =
        WorkspaceDirectory { .workspaces = { workspace(10, "api"), workspace(11, ""), workspace(12, "web") },
                             .selected = 11 };
    auto const choices = moveTargetsOf(directory);
    REQUIRE(choices.size() == 2);
    CHECK(choices[0].position == 1);
    CHECK(choices[0].title == "api");
    CHECK(choices[1].position == 3);
    CHECK(choices[1].title == "web");
}

TEST_CASE("an unlabelled workspace is named by its position", "[sidebarmenu]")
{
    auto const directory =
        WorkspaceDirectory { .workspaces = { workspace(10), workspace(11) }, .selected = 11 };
    auto const choices = moveTargetsOf(directory);
    REQUIRE(choices.size() == 1);
    CHECK(choices[0].title == "Workspace 1");
}

TEST_CASE("a window that follows no workspace can move a tab to any", "[sidebarmenu]")
{
    auto const directory =
        WorkspaceDirectory { .workspaces = { workspace(10), workspace(11) }, .selected = std::nullopt };
    CHECK(moveTargetsOf(directory).size() == 2);
}
