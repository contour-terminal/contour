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
        R"("visibility":"left","clients":0,"tabs":[]},)"
        R"({"id":"workspace:00000000-0000-0000-0000-00000000beef","label":"","position":2,"native":"9",)"
        R"("visibility":"left","clients":0,"tabs":[]}]})");
    REQUIRE(listed.has_value());
    using contour::cli::resolveWorkspaceSpec;
    CHECK(resolveWorkspaceSpec("018f3c2a", *listed) == 7); // an id suffix
    CHECK(resolveWorkspaceSpec("ops", *listed) == 7);      // its label
    CHECK(resolveWorkspaceSpec("2", *listed) == 9);        // its position
    CHECK(resolveWorkspaceSpec("workspace:00000000-0000-0000-0000-00000000beef", *listed) == 9);
    CHECK_FALSE(resolveWorkspaceSpec("nope", *listed).has_value());
}

TEST_CASE("--workspace needs a pinned certificate to list the daemon's workspaces", "[cli][switch]")
{
    CHECK(contour::cli::requireVerifiedForWorkspaceListing(vthost::PeerTrust::Local).has_value());
    CHECK(contour::cli::requireVerifiedForWorkspaceListing(vthost::PeerTrust::Verified).has_value());
    auto const refused = contour::cli::requireVerifiedForWorkspaceListing(vthost::PeerTrust::Unverified);
    REQUIRE(!refused.has_value());
    CHECK(refused.error().contains("--tls-ca"));
}
