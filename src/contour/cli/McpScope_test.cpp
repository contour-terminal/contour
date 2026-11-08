// SPDX-License-Identifier: Apache-2.0
//
// `contour mcp` own-tab scoping: what an agent may name, and the workspace as it sees it.

#include <contour/cli/McpScope.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using contour::cli::resolvePane;
using contour::cli::ScopeError;
using contour::cli::TabScope;
using Kind = contour::cli::ScopeError::Kind;

namespace
{
constexpr auto A1 = "pane:018f3c4e-0000-7000-8000-00000000a001";
constexpr auto A2 = "pane:018f3c4e-0000-7000-8000-00000000a002";
constexpr auto B1 = "pane:018f3c4e-0000-7000-8000-00000000b001";
constexpr auto TabA = "tab:018f3c4e-0000-7000-8000-0000000000aa";
constexpr auto TabB = "tab:018f3c4e-0000-7000-8000-0000000000bb";

// One workspace: tab B listed first, then tab A {a1 "build", a2 "tests"}; tab B {b1 "build"}.
vtcontrol::ListReply workspace()
{
    auto tabB = vtcontrol::TabInfo { .id = TabB, .position = 1 };
    tabB.panes.push_back(vtcontrol::PaneInfo { .id = B1, .label = "build" });
    auto tabA = vtcontrol::TabInfo { .id = TabA, .position = 2 };
    tabA.panes.push_back(vtcontrol::PaneInfo { .id = A1, .label = "build" });
    tabA.panes.push_back(vtcontrol::PaneInfo { .id = A2, .label = "tests" });
    auto workspace =
        vtcontrol::WorkspaceInfo { .id = "workspace:018f3c4e-0000-7000-8000-0000000000ff", .position = 1 };
    workspace.tabs.push_back(tabB);
    workspace.tabs.push_back(tabA);
    return { .workspaces = { workspace } };
}
} // namespace

TEST_CASE("OwnTab resolves only against the caller's tab", "[mcp-scope]")
{
    auto const ws = workspace();
    auto const label = resolvePane(ws, A1, "build", TabScope::OwnTab);
    REQUIRE(label);
    CHECK(*label == A1); // not ambiguous with b1
    CHECK(*resolvePane(ws, A1, "tests", TabScope::OwnTab) == A2);
    CHECK(*resolvePane(ws, A1, A2, TabScope::OwnTab) == A2);
    CHECK(*resolvePane(ws, A1, "a002", TabScope::OwnTab) == A2);
}

TEST_CASE("OwnTab refuses panes of other tabs", "[mcp-scope]")
{
    auto const ws = workspace();
    for (auto const* target: { B1, "b001", "pane:018f3c4e-0000-7000-8000-00000000b001" })
    {
        auto const r = resolvePane(ws, A1, target, TabScope::OwnTab);
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == Kind::OutsideTab);
        CHECK(r.error().message.contains("--allow-all-tabs"));
    }
    auto const missing = resolvePane(ws, A1, "nonesuch", TabScope::OwnTab);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().kind == Kind::NotFound);
}

TEST_CASE("AllTabs reaches every pane", "[mcp-scope]")
{
    auto const ws = workspace();
    CHECK(*resolvePane(ws, A1, B1, TabScope::AllTabs) == B1);
    CHECK(*resolvePane(ws, A1, "b001", TabScope::AllTabs) == B1);
    auto const ambiguous = resolvePane(ws, A1, "build", TabScope::AllTabs);
    REQUIRE_FALSE(ambiguous);
    CHECK(ambiguous.error().kind == Kind::Ambiguous);
}

TEST_CASE("an empty target is the caller's pane", "[mcp-scope]")
{
    auto const ws = workspace();
    CHECK(*resolvePane(ws, A1, "", TabScope::OwnTab) == A1);
    CHECK(*resolvePane(ws, A1, "", TabScope::AllTabs) == A1);
    for (auto const scope: { TabScope::OwnTab, TabScope::AllTabs })
    {
        auto const r = resolvePane(ws, "", "", scope);
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == Kind::NoCaller);
    }
}

TEST_CASE("a caller outside the workspace is gone", "[mcp-scope]")
{
    auto const ws = workspace();
    auto const r = resolvePane(ws, "pane:018f3c4e-0000-7000-8000-00000000dead", "tests", TabScope::OwnTab);
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == Kind::CallerGone);
}

TEST_CASE("ownTabOf names the caller's tab", "[mcp-scope]")
{
    auto const ws = workspace();
    CHECK(contour::cli::ownTabOf(ws, A2) == TabA);
    CHECK(contour::cli::ownTabOf(ws, B1) == TabB);
    CHECK_FALSE(contour::cli::ownTabOf(ws, "pane:nope"));
    CHECK_FALSE(contour::cli::ownTabOf(ws, ""));
}

TEST_CASE("annotateWorkspace marks self and sorts the own tab first", "[mcp-scope]")
{
    auto const json = contour::cli::annotateWorkspace(workspace(), A1);
    auto const self = std::string(R"("self":{"pane":")") + A1 + R"(","tab":")" + TabA + R"(","workspace":)";
    CHECK(json.contains(self));
    CHECK(json.find(TabA) < json.find(std::string(R"("id":")") + TabB));
    CHECK(json.contains(R"("isOwnTab":true)"));
    auto const first = json.find(R"("isSelf":true)");
    REQUIRE(first != std::string::npos);
    CHECK(json.find(R"("isSelf":true)", first + 1) == std::string::npos);
    CHECK(json.find(R"("isOwnTab":true)", json.find(R"("isOwnTab":true)") + 1) == std::string::npos);
    // isSelf belongs to a1, not a2 or b1.
    CHECK(json.find(R"("isSelf")") > json.find(std::string(R"("id":")") + A1));
    CHECK(json.find(R"("isSelf")") < json.find(std::string(R"("id":")") + A2));
}
