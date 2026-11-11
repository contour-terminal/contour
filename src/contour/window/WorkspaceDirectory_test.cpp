// SPDX-License-Identifier: Apache-2.0
#include <contour/window/WorkspaceDirectory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>

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

struct TableLabels final: LabelLookup
{
    std::map<uint64_t, std::string> table;
    [[nodiscard]] std::string labelOf(uint64_t workspace) const override
    {
        auto const found = table.find(workspace);
        return found == table.end() ? std::string {} : found->second;
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
    auto const directory = directoryFromLayouts(layouts, TableStatus {}, TableLabels {}, std::nullopt);
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
    auto const directory = directoryFromLayouts(layouts, TableStatus {}, TableLabels {}, std::nullopt);
    auto keys = std::vector<uint64_t> {};
    for (auto const& pane: directory.workspaces.at(0).tabs.at(0).panes)
        keys.push_back(pane.key.value);
    CHECK(keys == std::vector<uint64_t> { 1, 2, 3 });
}

TEST_CASE("a workspace's label comes from the label lookup, else empty", "[contour][directory]")
{
    auto const layouts = std::vector { layoutOf(1, {}), layoutOf(2, {}) };
    auto labels = TableLabels {};
    labels.table[2] = "build";
    auto const directory = directoryFromLayouts(layouts, TableStatus {}, labels, std::nullopt);
    REQUIRE(directory.workspaces.size() == 2);
    CHECK(directory.workspaces[0].label.empty());
    CHECK(directory.workspaces[1].label == "build");
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
    auto const directory = directoryFromLayouts(layouts, status, TableLabels {}, std::nullopt);
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

TEST_CASE("the selection is carried", "[contour][directory]")
{
    auto const layouts = std::vector { layoutOf(4, {}) };
    CHECK(directoryFromLayouts(layouts, TableStatus {}, TableLabels {}, 4).selected == 4);
    CHECK_FALSE(
        directoryFromLayouts(layouts, TableStatus {}, TableLabels {}, std::nullopt).selected.has_value());
}
