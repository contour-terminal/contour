// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vthost/WorkspaceVisibility.hpp>

using namespace vthost;

TEST_CASE("a visibility is named as the control API spells it", "[vthost][visibility]")
{
    CHECK(nameOf(WorkspaceVisibility::Shown) == "shown");
    CHECK(nameOf(WorkspaceVisibility::Hidden) == "hidden");
}

TEST_CASE("the visibility enumerators keep zero for Hidden", "[vthost][visibility]")
{
    STATIC_CHECK(WorkspaceVisibility {} == WorkspaceVisibility::Hidden);
}
