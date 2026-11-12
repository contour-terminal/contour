// SPDX-License-Identifier: Apache-2.0
#include <contour/session/InProcessDirectory.hpp>

#include <vtbackend/vt/ProgramStatus.hpp>

#include <core/async/testing/ManualExecutor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vtworkspace/ModelFixture.hpp>

using namespace contour;
using contour::session::InProcessDirectory;
using contour::session::InProcessWindow;
using contour::session::WorkspaceLabels;
using vtworkspace::SessionId;
using vtworkspace::WorkspaceId;

namespace
{

/// A window that owns what a test says and records what it was asked to show.
struct FakeWindow final: InProcessWindow
{
    std::vector<WorkspaceId> owned;
    WorkspaceId displayed {};
    std::vector<WorkspaceId> shown;

    [[nodiscard]] std::span<WorkspaceId const> ownedWorkspaces() const noexcept override { return owned; }
    [[nodiscard]] WorkspaceId displayedWorkspace() const noexcept override { return displayed; }
    void displayWorkspace(WorkspaceId workspace) override
    {
        shown.push_back(workspace);
        displayed = workspace;
    }
};

struct TableStatus final: window::PaneStatusLookup
{
    std::map<uint64_t, window::PaneStatus> table;
    [[nodiscard]] std::optional<window::PaneStatus> statusOf(window::PaneKey pane) const override
    {
        auto const found = table.find(pane.value);
        return found == table.end() ? std::nullopt : std::optional { found->second };
    }
};

struct FakeProbe final: vthost::IGitStatusProbe
{
    [[nodiscard]] std::optional<vthost::GitInfo> probe(std::filesystem::path const& /*cwd*/) const override
    {
        return vthost::GitInfo { .branch = "main", .ahead = 2, .behind = 1 };
    }
};

struct Harness
{
    vtworkspace::test::Fixture model;
    FakeWindow window;
    TableStatus status;
    WorkspaceLabels labels;
    int changes = 0;
    InProcessDirectory source { model.model, window, status, labels, nullptr, [this] { ++changes; } };

    /// Adds a workspace the window owns, with one tab; the first one is shown.
    vtworkspace::Workspace& addWorkspace()
    {
        auto* const workspace = model.model.createWorkspace();
        std::ignore = model.model.createTab(workspace->id());
        window.owned.push_back(workspace->id());
        if (window.owned.size() == 1)
            window.displayed = workspace->id();
        return *workspace;
    }
};

} // namespace

TEST_CASE("two in-process workspaces list in model order with labels", "[directory][inprocess]")
{
    auto h = Harness {};
    auto const& first = h.addWorkspace();
    auto const& second = h.addWorkspace();
    h.labels.set(first.id(), "build");
    h.labels.set(second.id(), "docs");

    auto const directory = h.source.directory();
    REQUIRE(directory.workspaces.size() == 2);
    CHECK(directory.workspaces[0].id == first.id().value);
    CHECK(directory.workspaces[0].label == "build");
    CHECK(directory.workspaces[1].id == second.id().value);
    CHECK(directory.workspaces[1].label == "docs");
    CHECK(directory.workspaces[0].tabs.size() == 1);
    CHECK(directory.selected == first.id().value);
    // An in-process workspace is in no object registry.
    CHECK(directory.workspaces[0].controlId == std::array<uint8_t, 16> {});
}

TEST_CASE("the directory follows the model's order, not the order the window took its workspaces in",
          "[directory][inprocess]")
{
    auto h = Harness {};
    auto const first = h.addWorkspace().id();
    auto const second = h.addWorkspace().id();
    // The window took the second workspace before the first.
    std::ranges::reverse(h.window.owned);

    auto const directory = h.source.directory();
    REQUIRE(directory.workspaces.size() == 2);
    CHECK(directory.workspaces[0].id == first.value);
    CHECK(directory.workspaces[1].id == second.value);
}

TEST_CASE("a workspace the window does not own is not listed", "[directory][inprocess]")
{
    auto h = Harness {};
    h.addWorkspace();
    std::ignore = h.model.model.createWorkspace();
    CHECK(h.source.directory().workspaces.size() == 1);
}

TEST_CASE("a split tab lists its panes depth-first", "[directory][inprocess]")
{
    auto h = Harness {};
    auto& workspace = h.addWorkspace();
    auto const tab = workspace.activeTab()->id();
    auto const firstPane = workspace.activeTab()->activePane()->id();
    REQUIRE(h.model.model.splitActivePane(tab, vtworkspace::SplitState::Vertical) != nullptr);
    // The first pane splits after the second one exists: its new neighbour is made last and sits second.
    h.model.model.setActivePane(tab, firstPane);
    REQUIRE(h.model.model.splitActivePane(tab, vtworkspace::SplitState::Horizontal) != nullptr);
    h.model.model.setTabTitle(tab, "dev");

    auto const directory = h.source.directory();
    REQUIRE(directory.workspaces.size() == 1);
    REQUIRE(directory.workspaces[0].tabs.size() == 1);
    auto const& tabRow = directory.workspaces[0].tabs[0];
    CHECK(tabRow.title == "dev");
    CHECK(tabRow.label == "dev");
    // Sessions are minted in creation order (1000, 1001, 1002); the tree reads 1000, 1002, 1001.
    REQUIRE(tabRow.panes.size() == 3);
    auto keys = std::vector<uint64_t> {};
    for (auto const& pane: tabRow.panes)
        keys.push_back(pane.key.value);
    CHECK(keys == std::vector<uint64_t> { 1000, 1002, 1001 });
}

TEST_CASE("program status of an in-process pane appears", "[directory][inprocess]")
{
    auto h = Harness {};
    auto& workspace = h.addWorkspace();
    auto const session = workspace.activeTab()->activePane()->session();
    auto record = vtbackend::ProgramStatusRecord {};
    record.state = vtbackend::ProgramState::Blocked;
    record.title = "needs you";
    auto known = window::PaneStatus {};
    known.title = "vim";
    known.cwd = "/src";
    known.status = { record };
    h.status.table[session.value] = std::move(known);

    auto const directory = h.source.directory();
    auto const& pane = directory.workspaces.at(0).tabs.at(0).panes.at(0);
    CHECK(pane.key.value == session.value);
    CHECK(pane.title == "vim");
    CHECK(pane.cwd == "/src");
    REQUIRE(pane.status.size() == 1);
    CHECK(pane.status[0].title == "needs you");
    CHECK(directory.workspaces[0].tabs[0].label == "vim");
}

TEST_CASE("a label set through the in-process store appears", "[directory][inprocess]")
{
    auto h = Harness {};
    auto const& workspace = h.addWorkspace();
    CHECK(h.source.directory().workspaces.at(0).label.empty());

    h.labels.set(workspace.id(), "renamed");
    CHECK(h.source.directory().workspaces.at(0).label == "renamed");

    h.labels.forget(workspace.id());
    CHECK(h.source.directory().workspaces.at(0).label.empty());
}

TEST_CASE("the git state the tracker holds appears on its workspace", "[directory][inprocess][git]")
{
    auto h = Harness {};
    auto const& workspace = h.addWorkspace();

    auto probe = FakeProbe {};
    auto executor = core::async::testing::ManualExecutor {};
    auto posted = std::deque<std::function<void()>> {};
    auto tracker = vthost::WorkspaceGitTracker {
        probe,
        executor,
        [&](std::function<void()> job) { posted.push_back(std::move(job)); },
        [](WorkspaceId, std::optional<vthost::GitInfo> const&) {},
    };
    auto const source = InProcessDirectory { h.model.model, h.window, h.status, h.labels, &tracker, {} };
    CHECK_FALSE(source.directory().workspaces.at(0).git.has_value());

    tracker.refresh(workspace.id(), "/src/contour");
    executor.drain();
    for (auto& job: posted)
        job();

    auto const git = source.directory().workspaces.at(0).git;
    REQUIRE(git.has_value());
    CHECK(git->branch == "main");
    CHECK(git->ahead == 2);
    CHECK(git->behind == 1);
}

TEST_CASE("select shows the workspace in the window", "[directory][inprocess]")
{
    auto h = Harness {};
    h.addWorkspace();
    auto const& second = h.addWorkspace();

    h.source.select(second.id().value);
    REQUIRE(h.window.shown.size() == 1);
    CHECK(h.window.shown[0] == second.id());
    CHECK(h.source.directory().selected == second.id().value);
}

TEST_CASE("notifyChanged tells the callback given at construction", "[directory][inprocess]")
{
    auto h = Harness {};
    h.source.notifyChanged();
    h.source.notifyChanged();
    CHECK(h.changes == 2);
}
