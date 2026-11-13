// SPDX-License-Identifier: Apache-2.0
#include <contour/window/WorkspaceDirectory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>
#include <utility>

using namespace contour::window;
namespace proto = vthost::proto;

namespace
{

struct TableStatus final: PaneStatusLookup
{
    std::map<uint64_t, PaneStatus> table;
    [[nodiscard]] std::optional<PaneStatus> statusOf(PaneKey pane) const override
    {
        auto const found = table.find(pane.value);
        return found == table.end() ? std::nullopt : std::optional { found->second };
    }
};

struct TableMeta final: WorkspaceMetaLookup
{
    std::map<uint64_t, WorkspaceMetadata> table;
    [[nodiscard]] WorkspaceMetadata metaOf(uint64_t workspace) const override
    {
        auto const found = table.find(workspace);
        return found == table.end() ? WorkspaceMetadata {} : found->second;
    }
};

proto::WirePane leaf(uint64_t session)
{
    return proto::WirePane { .paneId = session, .session = session };
}

proto::WirePane split(proto::WirePane first, proto::WirePane second)
{
    return proto::WirePane { .paneId = 100, .split = 1, .children = { std::move(first), std::move(second) } };
}

proto::LayoutState layoutOf(uint64_t workspace, std::vector<proto::WireTab> tabs)
{
    return proto::LayoutState { .workspace = workspace, .tabs = std::move(tabs) };
}

} // namespace

TEST_CASE("an unsplit tab lists one pane", "[contour][directory]")
{
    auto const layouts = std::vector { layoutOf(1, { proto::WireTab { .tabId = 7, .root = leaf(11) } }) };
    auto const directory = directoryFromLayouts(layouts, TableStatus {}, TableMeta {}, std::nullopt);
    REQUIRE(directory.workspaces.size() == 1);
    REQUIRE(directory.workspaces[0].tabs.size() == 1);
    CHECK(directory.workspaces[0].id == 1);
    CHECK(directory.workspaces[0].tabs[0].id == 7);
    REQUIRE(directory.workspaces[0].tabs[0].panes.size() == 1);
    CHECK(directory.workspaces[0].tabs[0].panes[0].key == PaneKey { 11 });
}

TEST_CASE("a split tab lists its panes depth-first", "[contour][directory]")
{
    auto const root = split(split(leaf(1), leaf(2)), leaf(3));
    auto const layouts = std::vector { layoutOf(1, { proto::WireTab { .tabId = 1, .root = root } }) };
    auto const directory = directoryFromLayouts(layouts, TableStatus {}, TableMeta {}, std::nullopt);
    auto keys = std::vector<uint64_t> {};
    for (auto const& pane: directory.workspaces.at(0).tabs.at(0).panes)
        keys.push_back(pane.key.value);
    CHECK(keys == std::vector<uint64_t> { 1, 2, 3 });
}

TEST_CASE("a workspace's label, control id and git state come from the metadata lookup, else empty",
          "[contour][directory]")
{
    auto const layouts = std::vector { layoutOf(1, { proto::WireTab { .tabId = 1, .root = leaf(1) } }),
                                       layoutOf(2, { proto::WireTab { .tabId = 2, .root = leaf(2) } }) };
    auto metadata = TableMeta {};
    auto const git =
        DirectoryGit { .branch = "main", .kind = vtbackend::GitHeadKind::Branch, .ahead = 1, .behind = {} };
    metadata.table[2] = WorkspaceMetadata {
        .label = "build",
        .controlId = { 0xAB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02 },
        .git = git,
    };
    auto const directory = directoryFromLayouts(layouts, TableStatus {}, metadata, std::nullopt);
    REQUIRE(directory.workspaces.size() == 2);
    CHECK(directory.workspaces[0].label.empty());
    CHECK(directory.workspaces[0].controlId == std::array<uint8_t, 16> {});
    CHECK_FALSE(directory.workspaces[0].git.has_value());
    CHECK(directory.workspaces[1].label == "build");
    CHECK(directory.workspaces[1].controlId == metadata.table[2].controlId);
    CHECK(directory.workspaces[1].git == std::optional { git });
}

TEST_CASE("status and summaries come from the lookup, keyed by daemon session id", "[contour][directory]")
{
    auto const layouts =
        std::vector { layoutOf(1,
                               { proto::WireTab { .tabId = 1, .root = split(leaf(5), leaf(6)) },
                                 proto::WireTab { .tabId = 2, .title = "named", .root = leaf(9) } }) };
    auto record = vtbackend::ProgramStatusRecord {};
    record.id = "build";
    auto status = TableStatus {};
    status.table[5] = PaneStatus { .title = "vim",
                                   .cwd = "/src",
                                   .status = { record },
                                   .activity = proto::OutputActivity::Output,
                                   .finished = proto::FinishedCommand { .exitCode = 2 } };
    auto const directory = directoryFromLayouts(layouts, status, TableMeta {}, std::nullopt);
    auto const& tabs = directory.workspaces.at(0).tabs;
    auto const& known = tabs.at(0).panes.at(0);
    CHECK(known.title == "vim");
    CHECK(known.cwd == "/src");
    REQUIRE(known.status.size() == 1);
    CHECK(known.status[0].id == "build");
    CHECK(known.activity == proto::OutputActivity::Output);
    REQUIRE(known.finished.has_value());
    CHECK(known.finished->exitCode == 2);
    CHECK(tabs[0].label == "vim"); // no title of its own: the first pane's
    // A pane nothing is known of is empty, and a tab's own title wins.
    CHECK(tabs[0].panes[1].title.empty());
    CHECK(tabs[0].panes[1].activity == proto::OutputActivity::None);
    CHECK(tabs[1].label == "named");
}

TEST_CASE("a tab's control label names it over its title and its first pane's", "[contour][directory]")
{
    auto const layouts = std::vector { layoutOf(
        1,
        { proto::WireTab { .tabId = 1, .title = "named", .label = "build", .root = leaf(5) },
          proto::WireTab { .tabId = 2, .label = "logs", .root = leaf(6) },
          proto::WireTab { .tabId = 3, .title = "named", .root = leaf(7) } }) };
    auto status = TableStatus {};
    for (auto const session: { uint64_t { 5 }, uint64_t { 6 }, uint64_t { 7 } })
    {
        auto pane = PaneStatus {};
        pane.title = "vim";
        status.table[session] = std::move(pane);
    }
    auto const directory = directoryFromLayouts(layouts, status, TableMeta {}, std::nullopt);
    auto const& tabs = directory.workspaces.at(0).tabs;
    REQUIRE(tabs.size() == 3);
    CHECK(tabs[0].label == "build");
    CHECK(tabs[0].title == "named");
    CHECK(tabs[1].label == "logs");
    CHECK(tabs[2].label == "named");
}

TEST_CASE("the selection is carried", "[contour][directory]")
{
    auto const layouts = std::vector { layoutOf(4, {}) };
    CHECK(directoryFromLayouts(layouts, TableStatus {}, TableMeta {}, 4).selected == 4);
    CHECK_FALSE(
        directoryFromLayouts(layouts, TableStatus {}, TableMeta {}, std::nullopt).selected.has_value());
}

TEST_CASE("a workspace's title is its label, else its host's 1-based number for it", "[contour][directory]")
{
    auto workspace = DirectoryWorkspace {};
    SECTION("its host stated its place: numbered as `contour ls` numbers it, not by its place in the list")
    {
        workspace.position = 2;
        CHECK(workspaceTitleOf(workspace, 1) == "Workspace 3");
    }
    SECTION("its host stated none: its place in the list")
    {
        CHECK(workspaceTitleOf(workspace, 1) == "Workspace 2");
    }
    SECTION("a label wins")
    {
        workspace.label = "ops";
        workspace.position = 2;
        CHECK(workspaceTitleOf(workspace, 1) == "ops");
    }
}

namespace
{
// Whether each lookup takes a directory of that type: an lvalue does, a temporary -- whose workspaces and
// tabs a result would point into after it is gone -- does not.
template <typename Directory>
concept FindsWorkspaceIn =
    requires(Directory&& directory) { findWorkspace(std::forward<Directory>(directory), 1U); };
template <typename Directory>
concept FindsTabIn = requires(Directory&& directory) { findTab(std::forward<Directory>(directory), 1U, 1U); };
template <typename Directory>
concept FindsTabAnywhereIn =
    requires(Directory&& directory) { findTabAnywhere(std::forward<Directory>(directory), 1U); };
template <typename Directory>
concept FindsPaneIn =
    requires(Directory&& directory) { tabHoldingPane(std::forward<Directory>(directory), PaneKey { 1 }); };
template <typename Workspace>
concept FindsTabOf = requires(Workspace&& workspace) { findTab(std::forward<Workspace>(workspace), 1U); };
} // namespace

static_assert(FindsWorkspaceIn<WorkspaceDirectory const&> && !FindsWorkspaceIn<WorkspaceDirectory>);
static_assert(FindsTabIn<WorkspaceDirectory const&> && !FindsTabIn<WorkspaceDirectory>);
static_assert(FindsTabAnywhereIn<WorkspaceDirectory const&> && !FindsTabAnywhereIn<WorkspaceDirectory>);
static_assert(FindsPaneIn<WorkspaceDirectory const&> && !FindsPaneIn<WorkspaceDirectory>);
static_assert(FindsTabOf<DirectoryWorkspace const&> && !FindsTabOf<DirectoryWorkspace>);

TEST_CASE("WorkspaceDirectory: a workspace, a tab and a pane's tab are looked up by id", "[directory]")
{
    auto const tabOf = [](uint64_t id, std::vector<uint64_t> const& panes) {
        auto tab = DirectoryTab {};
        tab.id = id;
        for (auto const pane: panes)
            tab.panes.emplace_back().key = PaneKey { pane };
        return tab;
    };
    auto build = DirectoryWorkspace {};
    build.id = 10;
    build.tabs = { tabOf(100, { 1000, 1001 }), tabOf(101, { 1002 }) };
    auto docs = DirectoryWorkspace {};
    docs.id = 20;
    docs.tabs = { tabOf(200, { 2000 }) };
    auto const directory = WorkspaceDirectory { .workspaces = { build, docs }, .selected = 10 };

    REQUIRE(findWorkspace(directory, 20) != nullptr);
    CHECK(findWorkspace(directory, 20)->id == 20);
    CHECK(findWorkspace(directory, 30) == nullptr);

    auto const tab = findTab(directory, 10, 101);
    REQUIRE(tab);
    CHECK(tab->workspace->id == 10);
    CHECK(tab->tab->id == 101);
    CHECK_FALSE(findTab(directory, 20, 101)); // listed, but by another workspace
    CHECK_FALSE(findTab(directory, 30, 101));

    auto const anywhere = findTabAnywhere(directory, 200);
    REQUIRE(anywhere);
    CHECK(anywhere->workspace->id == 20);
    CHECK_FALSE(findTabAnywhere(directory, 999));

    auto const holding = tabHoldingPane(directory, PaneKey { 1001 });
    REQUIRE(holding);
    CHECK(holding->workspace->id == 10);
    CHECK(holding->tab->id == 100);
    CHECK_FALSE(tabHoldingPane(directory, PaneKey { 9 }));

    CHECK(sessionsOf(directory.workspaces[0].tabs) == std::vector<uint64_t> { 1000, 1001, 1002 });
    CHECK(sessionsOf({}).empty());
}
