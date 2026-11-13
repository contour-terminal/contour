// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/WorkspaceListing.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("decodeWorkspaceList reads a workspace.list reply", "[cli][switch]")
{
    auto const listed = contour::cli::decodeWorkspaceList(
        R"({"workspaces":[{"id":"workspace:00000000-0000-0000-0000-000000000001","label":"ops","position":1,)"
        R"("native":"7","visibility":"hidden","clients":0,"tabs":[]}]})");
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().label == "ops");
    CHECK(listed->front().native == "7");
    CHECK(listed->front().visibility == "hidden");
}

TEST_CASE("decodeWorkspaceList says why a reply is no listing", "[cli][switch]")
{
    auto const listed = contour::cli::decodeWorkspaceList(R"({"workspaces":7})");
    REQUIRE_FALSE(listed.has_value());
    CHECK_FALSE(listed.error().empty());
}

// Review (PR 3, item 10): `contour client --workspace` took only the daemon's internal numeric id, which
// nothing shows the user.
TEST_CASE("resolveWorkspaceSpec takes what the control verbs take", "[cli][switch]")
{
    auto const listed = contour::cli::decodeWorkspaceList(
        R"({"workspaces":[)"
        R"({"id":"workspace:00000000-0000-0000-0000-0000018f3c2a","label":"ops","position":1,"native":"7",)"
        R"("visibility":"hidden","clients":0,"tabs":[]},)"
        R"({"id":"workspace:00000000-0000-0000-0000-00000000beef","label":"","position":2,"native":"9",)"
        R"("visibility":"hidden","clients":0,"tabs":[]}]})");
    REQUIRE(listed.has_value());
    using contour::cli::resolveWorkspaceSpec;
    CHECK(resolveWorkspaceSpec("018f3c2a", *listed) == 7); // an id suffix
    CHECK(resolveWorkspaceSpec("ops", *listed) == 7);      // its label
    CHECK(resolveWorkspaceSpec("2", *listed) == 9);        // its position
    CHECK(resolveWorkspaceSpec("workspace:00000000-0000-0000-0000-00000000beef", *listed) == 9);
    CHECK_FALSE(resolveWorkspaceSpec("nope", *listed).has_value());
}

TEST_CASE("workspace.create is asked with the label and answers the new workspace's id", "[cli][switch]")
{
    CHECK(contour::cli::encodeCreateWorkspace("q") == R"({"cwd":"","label":"q"})");
    auto const made = contour::cli::decodeCreatedWorkspace(
        R"({"workspace":"workspace:018f3c4e-9a1b-7c2d-8e3f-0123456789ab","tab":"tab:018f3c4e-9a1b-7c2d-8e3f-0123456789ac","pane":"pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ad"})");
    REQUIRE(made.has_value());
    CHECK(vtcontrol::toString(*made) == "workspace:018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK_FALSE(contour::cli::decodeCreatedWorkspace(R"({"workspace":"","tab":"","pane":""})").has_value());
    CHECK_FALSE(contour::cli::decodeCreatedWorkspace("not json").has_value());
}
