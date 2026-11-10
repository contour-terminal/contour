// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SwitcherRows.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <vtcontrol/ObjectRef.hpp>

using contour::session::SwitchTarget;
using contour::window::SwitcherDaemon;
using contour::window::SwitcherExpansion;
using contour::window::SwitcherGroup;
using contour::window::SwitcherHostLink;
using contour::window::switcherInitialSelection;
using contour::window::SwitcherInputs;
using contour::window::SwitcherListing;
using contour::window::SwitcherRow;
using contour::window::SwitcherRowKind;
using contour::window::SwitcherScope;
using contour::window::SwitcherWorkspace;
using contour::window::SwitcherWorkspaceState;
using vthost::proto::SwitchDaemonKind;

namespace
{

/// @return A workspace's control id, made from @p name: equal for equal names.
[[nodiscard]] vtcontrol::ObjectRef workspaceRef(std::string_view name)
{
    auto ref = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace, .uuid = {} };
    for (auto const [index, character]: std::views::zip(std::views::iota(std::size_t { 0 }), name))
        ref.uuid.bytes.at(index % ref.uuid.bytes.size()) ^= static_cast<uint8_t>(character);
    return ref;
}

[[nodiscard]] SwitchTarget label(std::string name, contour::session::WorkspaceTarget workspace = {})
{
    return SwitchTarget { .kind = SwitchDaemonKind::Label,
                          .daemon = std::move(name),
                          .workspace = std::move(workspace) };
}

[[nodiscard]] SwitchTarget host(std::string name)
{
    return SwitchTarget { .kind = SwitchDaemonKind::Host, .daemon = std::move(name), .workspace = {} };
}

[[nodiscard]] SwitcherWorkspace workspace(uint32_t position,
                                          std::string name,
                                          std::size_t tabs,
                                          SwitcherWorkspaceState state)
{
    return SwitcherWorkspace {
        .ref = workspaceRef(name), .position = position, .name = std::move(name), .tabs = tabs, .state = state
    };
}

/// The mockup's panel B: `default` (this window's, expanded, two workspaces), `admin` (expanded, two) and
/// the saved host `studio-admin`, not yet connected.
[[nodiscard]] std::vector<SwitcherDaemon> mockup()
{
    return {
        SwitcherDaemon { .address = label("default"),
                         .workspaceCount = 2,
                         .tag = {},
                         .expansion = SwitcherExpansion::Expanded,
                         .listing = SwitcherListing::Listed,
                         .failure = {},
                         .workspaces = { workspace(1, "work", 4, SwitcherWorkspaceState::ThisWindow),
                                         workspace(3, "scratch", 1, SwitcherWorkspaceState::Left) } },
        SwitcherDaemon { .address = label("admin"),
                         .workspaceCount = 2,
                         .tag = {},
                         .expansion = SwitcherExpansion::Expanded,
                         .listing = SwitcherListing::Listed,
                         .failure = {},
                         .workspaces = { workspace(1, "ops", 3, SwitcherWorkspaceState::Hidden),
                                         workspace(2, "logs", 1, SwitcherWorkspaceState::ShownElsewhere) } },
        SwitcherDaemon { .address = host("studio-admin"),
                         .workspaceCount = std::nullopt,
                         .tag = "TLS",
                         .expansion = SwitcherExpansion::Collapsed,
                         .listing = SwitcherListing::NotListed,
                         .failure = {},
                         .workspaces = {} },
    };
}

[[nodiscard]] std::vector<std::string> titlesOf(std::vector<SwitcherRow> const& rows)
{
    auto titles = std::vector<std::string> {};
    for (auto const& row: rows)
        titles.push_back(row.title);
    return titles;
}

} // namespace

TEST_CASE("switcherRows groups the daemons and lists the expanded ones' workspaces", "[switcher]")
{
    auto const daemons = mockup();
    auto const rows =
        switcherRows(SwitcherInputs { .daemons = daemons, .windowDaemon = label("default") }, "");

    CHECK(titlesOf(rows)
          == std::vector<std::string> { "default",
                                        "1 \"work\"",
                                        "3 \"scratch\"",
                                        "+ new workspace on default",
                                        "admin",
                                        "1 \"ops\"",
                                        "2 \"logs\"",
                                        "+ new workspace on admin",
                                        "New daemon…",
                                        "studio-admin" });
    REQUIRE(rows.size() == 10);
    CHECK(rows[0].group == SwitcherGroup::ThisComputer);
    CHECK(rows[0].detail == "running · 2 workspaces · this window");
    CHECK(rows[0].expansion == SwitcherExpansion::Expanded);
    CHECK(rows[1].detail == "4 tabs · this window");
    CHECK(rows[2].detail == "1 tab · not shown");
    CHECK(rows[5].detail == "3 tabs · hidden");
    CHECK(rows[6].detail == "1 tab · shown elsewhere");
    CHECK(rows[4].detail == "running · 2 workspaces");
    CHECK(rows[8].kind == SwitcherRowKind::NewDaemon);
    CHECK(rows[9].group == SwitcherGroup::SavedHosts);
    CHECK(rows[9].detail == "not yet connected");
    CHECK(rows[9].tag == "TLS");
    CHECK(rows[9].expansion == SwitcherExpansion::Collapsed);

    // Where each row goes.
    CHECK(rows[0].target == label("default"));
    CHECK(rows[5].target == label("admin", workspaceRef("ops")));
    CHECK(rows[7].target == label("admin", contour::session::NewWorkspace {}));
    CHECK(rows[9].target == host("studio-admin"));
}

TEST_CASE("switcherRows says how a daemon's listing stands", "[switcher]")
{
    auto daemons = mockup();
    daemons[2].expansion = SwitcherExpansion::Expanded;

    SECTION("listing")
    {
        daemons[2].listing = SwitcherListing::Listing;
        auto const rows =
            switcherRows(SwitcherInputs { .daemons = daemons, .windowDaemon = label("default") }, "");
        CHECK(rows[9].detail == "listing…");
        // Nothing listed under it yet but the way to a new workspace.
        REQUIRE(rows.size() == 11);
        CHECK(rows[10].kind == SwitcherRowKind::NewWorkspace);
    }
    SECTION("failed")
    {
        daemons[2].listing = SwitcherListing::Failed;
        daemons[2].failure = "connection refused";
        auto const rows =
            switcherRows(SwitcherInputs { .daemons = daemons, .windowDaemon = label("default") }, "");
        CHECK(rows[9].detail == "could not list: connection refused");
    }
    SECTION("listed, and this window's daemon")
    {
        daemons[2].listing = SwitcherListing::Listed;
        daemons[2].workspaces = { workspace(1, "build", 2, SwitcherWorkspaceState::ThisWindow) };
        auto const rows =
            switcherRows(SwitcherInputs { .daemons = daemons, .windowDaemon = host("studio-admin") }, "");
        CHECK(rows[0].detail == "running · 2 workspaces");
        CHECK(rows[9].detail == "connected · 1 workspace · this window");
    }
}

TEST_CASE("switcherRows filters daemons and workspaces", "[switcher]")
{
    auto daemons = mockup();
    daemons[1].expansion = SwitcherExpansion::Collapsed;
    auto const rowsFor = [&](std::string_view filter) {
        return titlesOf(
            switcherRows(SwitcherInputs { .daemons = daemons, .windowDaemon = label("default") }, filter));
    };

    // A daemon's name keeps it, with what is expanded under it.
    CHECK(rowsFor("DEFAULT")
          == std::vector<std::string> {
              "default", "1 \"work\"", "3 \"scratch\"", "+ new workspace on default" });
    // A workspace's name finds it under a collapsed daemon too.
    CHECK(rowsFor("logs") == std::vector<std::string> { "admin", "2 \"logs\"" });
    CHECK(rowsFor("studio") == std::vector<std::string> { "studio-admin" });
    CHECK(rowsFor("new dae") == std::vector<std::string> { "New daemon…" });
    CHECK(rowsFor("nothing like it").empty());
}

TEST_CASE("switcherRows lists a pick's daemon alone", "[switcher]")
{
    auto const daemons = std::vector<SwitcherDaemon> { mockup()[1] };
    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons,
                                                    .windowDaemon = label("default"),
                                                    .scope = SwitcherScope::OneDaemon },
                                   "");

    CHECK(titlesOf(rows)
          == std::vector<std::string> { "1 \"ops\"", "2 \"logs\"", "+ new workspace on admin" });
    for (auto const& row: rows)
        CHECK(row.group == SwitcherGroup::Pick);
}

TEST_CASE("switcherSelection keeps the row it was on", "[switcher]")
{
    auto const daemons = mockup();
    auto const rows =
        switcherRows(SwitcherInputs { .daemons = daemons, .windowDaemon = label("default") }, "");
    CHECK(switcherSelection(rows, std::nullopt) == 0);
    CHECK(switcherSelection(rows, label("admin")) == 4);
    CHECK(switcherSelection(rows, label("gone")) == 0);
}

TEST_CASE("switcherInitialSelection starts on the first daemon that is not the window's own", "[switcher]")
{
    auto const daemons = mockup();
    auto const everything = [&](SwitchTarget const& own) {
        return switcherRows(SwitcherInputs { .daemons = daemons, .windowDaemon = own }, "");
    };

    SECTION("a window on the first daemon opens on the next one, not on New daemon")
    {
        auto const rows = everything(label("default"));
        auto const at = switcherInitialSelection(rows, label("default"));
        REQUIRE(at < rows.size());
        CHECK(rows[at].kind == SwitcherRowKind::Daemon);
        CHECK(rows[at].target.daemon != "default");
    }

    SECTION("a window on another daemon opens on the first one")
    {
        auto const rows = everything(label("admin"));
        auto const at = switcherInitialSelection(rows, label("admin"));
        REQUIRE(at < rows.size());
        CHECK(rows[at].kind == SwitcherRowKind::Daemon);
        CHECK(rows[at].target.daemon != "admin");
    }

    SECTION("the window's own daemon is the fallback when it is the only one")
    {
        auto const only = std::vector<SwitcherDaemon> { mockup()[0] };
        auto const rows =
            switcherRows(SwitcherInputs { .daemons = only, .windowDaemon = label("default") }, "");
        auto const at = switcherInitialSelection(rows, label("default"));
        REQUIRE(at < rows.size());
        CHECK(rows[at].kind == SwitcherRowKind::Daemon);
        CHECK(rows[at].target.daemon == "default");
    }

    SECTION("a saved host is never preselected while a local daemon can be")
    {
        auto const hostOnly = std::vector<SwitcherDaemon> { mockup()[0], mockup()[2] };
        auto const rows =
            switcherRows(SwitcherInputs { .daemons = hostOnly, .windowDaemon = label("default") }, "");
        auto const at = switcherInitialSelection(rows, label("default"));
        REQUIRE(at < rows.size());
        // The only other daemon is a saved host: the window's own row stays selected.
        CHECK(rows[at].target.kind == SwitchDaemonKind::Label);
        CHECK(rows[at].target.daemon == "default");
    }

    SECTION("without daemon rows it is the first row")
    {
        CHECK(switcherInitialSelection({}, label("default")) == 0);
    }
}

TEST_CASE("a saved host reached before says it is not connected, not that it was never connected",
          "[switcher]")
{
    auto daemons = mockup();
    auto const detailOfStudio = [&] {
        auto const rows =
            switcherRows(SwitcherInputs { .daemons = daemons, .windowDaemon = label("default") }, "");
        auto const studio = std::ranges::find(rows, "studio-admin", &SwitcherRow::title);
        REQUIRE(studio != rows.end());
        return studio->detail;
    };

    REQUIRE(daemons.back().address.kind == SwitchDaemonKind::Host);
    CHECK(detailOfStudio() == "not yet connected");
    daemons.back().link = SwitcherHostLink::Disconnected;
    CHECK(detailOfStudio() == "not connected");
    daemons.back().link = SwitcherHostLink::Connected;
    CHECK(detailOfStudio() == "connected");
}

TEST_CASE("switcherHostLinkOf tells a connected host from one reached before and one never reached",
          "[switcher]")
{
    using contour::window::switcherHostLinkOf;

    auto const reached = std::set<std::string> { "studio" };
    auto const shownHere = std::vector<std::string> { "build" };
    CHECK(switcherHostLinkOf("studio", reached, shownHere) == SwitcherHostLink::Disconnected);
    CHECK(switcherHostLinkOf("other", reached, shownHere) == SwitcherHostLink::NeverConnected);
    // A window on it holds a live link, whether or not the application started it from the switcher.
    CHECK(switcherHostLinkOf("build", reached, shownHere) == SwitcherHostLink::Connected);
    CHECK(switcherHostLinkOf("studio", reached, std::vector<std::string> { "studio" })
          == SwitcherHostLink::Connected);
}
