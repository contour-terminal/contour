// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Json.hpp>

TEST_CASE("API JSON keeps the member spelling", "[vtcontrol][api]")
{
    auto const json = vtcontrol::encodeJson(vtcontrol::SplitRequest {
        .pane = "pane:x", .direction = vtcontrol::SplitDirection::Below, .ratioPercent = 30 });
    CHECK(
        json
        == R"({"pane":"pane:x","direction":"below","ratioPercent":30,"cwd":"","command":[],"label":"","focus":"take"})");
    CHECK_FALSE(vtcontrol::decodeJson<vtcontrol::SplitRequest>(R"({"direction":"left"})"));
    CHECK_FALSE(vtcontrol::decodeJson<vtcontrol::SplitRequest>(R"({"ratio":0.3})"));
}

TEST_CASE("API replies round-trip through JSON", "[vtcontrol][api]")
{
    auto const reply = vtcontrol::ListReply {
        .workspaces = { vtcontrol::WorkspaceInfo {
            .id = "workspace:w",
            .tabs = { vtcontrol::TabInfo {
                .id = "tab:t",
                .isActive = true,
                .panes = { vtcontrol::PaneInfo { .id = "pane:p",
                                                 .size = { .columns = 80, .lines = 24 } } } } } } }
    };
    auto const back = vtcontrol::decodeJson<vtcontrol::ListReply>(vtcontrol::encodeJson(reply));
    REQUIRE(back);
    REQUIRE(back->workspaces.size() == 1);
    CHECK(back->workspaces[0].tabs[0].isActive);
    CHECK(back->workspaces[0].tabs[0].panes[0].size.columns == 80);
}

TEST_CASE("a workspace's git and a pane's last finished command round-trip through JSON", "[vtcontrol][api]")
{
    auto const reply = vtcontrol::ListReply {
        .workspaces = { vtcontrol::WorkspaceInfo {
            .id = "workspace:w",
            .tabs = { vtcontrol::TabInfo {
                .id = "tab:t",
                .panes = { vtcontrol::PaneInfo {
                    .id = "pane:p",
                    .lastFinished = vtcontrol::BlockInfo { .id = "block:p/3",
                                                           .pane = "pane:p",
                                                           .state = vtcontrol::BlockState::Finished,
                                                           .command = "make deploy",
                                                           .exitCode = 1,
                                                           .startedAt = "2026-11-12T10:00:00.000Z",
                                                           .durationMs = 1500 } } } } },
            .git = vtcontrol::WorkspaceGitInfo { .branch = "main",
                                                 .kind = vtcontrol::GitHeadKind::Branch,
                                                 .ahead = 2,
                                                 .behind = std::nullopt } } }
    };
    auto const json = vtcontrol::encodeJson(reply);
    CHECK(json.contains(R"("git":{"branch":"main","kind":"branch","ahead":2)"));
    auto const back = vtcontrol::decodeJson<vtcontrol::ListReply>(json);
    REQUIRE(back);
    REQUIRE(back->workspaces.size() == 1);
    auto const& workspace = back->workspaces[0];
    REQUIRE(workspace.git.has_value());
    CHECK(workspace.git->branch == "main");
    CHECK(workspace.git->kind == vtcontrol::GitHeadKind::Branch);
    CHECK(workspace.git->ahead == 2);
    CHECK_FALSE(workspace.git->behind.has_value());
    auto const& last = workspace.tabs[0].panes[0].lastFinished;
    REQUIRE(last.has_value());
    CHECK(last->command == "make deploy");
    CHECK(last->exitCode == 1);
    CHECK(last->durationMs == 1500);

    // Both are absent from a daemon that has neither to say.
    auto const bare = vtcontrol::decodeJson<vtcontrol::ListReply>(
        R"({"workspaces":[{"id":"workspace:w","tabs":[{"id":"tab:t","panes":[{"id":"pane:p"}]}]}]})");
    REQUIRE(bare);
    CHECK_FALSE(bare->workspaces[0].git.has_value());
    CHECK_FALSE(bare->workspaces[0].tabs[0].panes[0].lastFinished.has_value());
    CHECK(vtcontrol::nameOf(vtcontrol::GitHeadKind::OtherRef) == "otherRef");
}

TEST_CASE("DaemonInfoReply round-trips grants", "[vtcontrol][api]")
{
    auto const json = vtcontrol::encodeJson(vtcontrol::DaemonInfoReply { .grants = { "read", "screen" } });
    CHECK(json.contains(R"("grants":["read","screen"])"));
    auto const back = vtcontrol::decodeJson<vtcontrol::DaemonInfoReply>(json);
    REQUIRE(back);
    CHECK(back->grants == std::vector<std::string> { "read", "screen" });

    auto const old = vtcontrol::decodeJson<vtcontrol::DaemonInfoReply>(
        R"({"version":"0.9.0","protocolVersion":1,"platform":"linux","hostname":"devbox","uptimeSeconds":3600})");
    REQUIRE(old);
    CHECK(old->grants.empty());
}

TEST_CASE("an empty request decodes from nothing and from an empty object", "[vtcontrol][api]")
{
    CHECK(vtcontrol::decodeJson<vtcontrol::DaemonInfoRequest>(""));
    CHECK(vtcontrol::decodeJson<vtcontrol::DaemonInfoRequest>("{}"));
    CHECK_FALSE(vtcontrol::decodeJson<vtcontrol::DaemonInfoRequest>(R"({"x":1})"));
    CHECK(vtcontrol::encodeJson(vtcontrol::EmptyReply {}) == "{}");
}

TEST_CASE("ApiDescription carries schemas and examples as raw JSON", "[vtcontrol][api]")
{
    auto description = vtcontrol::ApiDescription { .name = "list" };
    description.inputSchema.text = R"({"type":"object"})";
    auto const json = vtcontrol::encodeJson(description);
    CHECK(json.contains(R"("inputSchema":{"type":"object"})"));
}

TEST_CASE("every API record is tagged", "[vtcontrol][api]")
{
    STATIC_REQUIRE(vtcontrol::ApiRecord<vtcontrol::SplitRequest>);
    STATIC_REQUIRE(vtcontrol::ApiRecord<vtcontrol::EmptyReply>);
    STATIC_REQUIRE(vtcontrol::ApiRecord<vtcontrol::BlockInfo>);
    STATIC_REQUIRE_FALSE(vtcontrol::ApiRecord<int>);
}

TEST_CASE("API enums are spelled by their tables", "[vtcontrol][api]")
{
    CHECK(vtcontrol::nameOf(vtcontrol::SendWait::CommandFinished) == "commandFinished");
    CHECK(vtcontrol::nameOf(vtcontrol::ProgramStatusReason::ProcessExit) == "processExit");
    CHECK(vtcontrol::nameOf(vtcontrol::ProgramStatusReason::UserInput) == "userInput");
    CHECK(vtcontrol::enumFromName<vtcontrol::BlockState>("superseded") == vtcontrol::BlockState::Superseded);
}
