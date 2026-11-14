// SPDX-License-Identifier: Apache-2.0
//
// The switcher's rows as palette levels (boards 1, 2, 3 and 5 of the redesign): each rule is a case over a
// fixture of plain data, with no window and no Qt.

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
using contour::window::SwitcherDiscovery;
using contour::window::SwitcherGroup;
using contour::window::SwitcherHostLink;
using contour::window::switcherInitialSelection;
using contour::window::SwitcherInputs;
using contour::window::SwitcherLevel;
using contour::window::SwitcherListing;
using contour::window::SwitcherOrigin;
using contour::window::SwitcherPill;
using contour::window::SwitcherRow;
using contour::window::SwitcherRowKind;
using contour::window::switcherRows;
using contour::window::SwitcherScope;
using contour::window::switcherSelection;
using contour::window::SwitcherStatus;
using contour::window::SwitcherView;
using contour::window::SwitcherWorkspace;
using contour::window::SwitcherWorkspaceState;
using contour::window::TabBadgeKind;
using contour::window::Urgency;
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
                                          SwitcherWorkspaceState state = SwitcherWorkspaceState::Hidden,
                                          SwitcherStatus status = {})
{
    return SwitcherWorkspace { .ref = workspaceRef(name),
                               .position = position,
                               .name = std::move(name),
                               .tabs = 1,
                               .state = state,
                               .status = std::move(status) };
}

[[nodiscard]] SwitcherStatus failed()
{
    auto status = SwitcherStatus {};
    status.urgency = Urgency::Failed;
    status.glyph = TabBadgeKind::Error;
    return status;
}

[[nodiscard]] SwitcherDaemon daemon(SwitchTarget address, std::vector<SwitcherWorkspace> workspaces)
{
    return SwitcherDaemon { .address = std::move(address),
                            .workspaceCount = workspaces.size(),
                            .tag = {},
                            .listing = SwitcherListing::Listed,
                            .failure = {},
                            .workspaces = std::move(workspaces) };
}

/// Boards 1 and 2: `work` is the window's daemon, holding four workspaces, the window on `contour`; `home`
/// is another local daemon.
[[nodiscard]] std::vector<SwitcherDaemon> boards()
{
    return {
        daemon(label("work"),
               { workspace(1, "contour", SwitcherWorkspaceState::ThisWindow),
                 workspace(2, "api-gateway"),
                 workspace(3, "docs", SwitcherWorkspaceState::ShownElsewhere),
                 workspace(4, "scratch") }),
        daemon(label("home"),
               { workspace(1, "api-docs", SwitcherWorkspaceState::Hidden, failed()),
                 workspace(2, "rapid-proto"),
                 workspace(3, "dotfiles") }),
    };
}

[[nodiscard]] std::vector<SwitcherRowKind> kindsOf(std::vector<SwitcherRow> const& rows)
{
    auto kinds = std::vector<SwitcherRowKind> {};
    for (auto const& row: rows)
        kinds.push_back(row.kind);
    return kinds;
}

[[nodiscard]] std::vector<std::string> titlesOf(std::vector<SwitcherRow> const& rows)
{
    auto titles = std::vector<std::string> {};
    for (auto const& row: rows)
        titles.push_back(row.title);
    return titles;
}

using enum SwitcherRowKind;

} // namespace

TEST_CASE("the top level lists the window's daemon's workspaces, the other daemons, then the actions",
          "[switcher]")
{
    auto const daemons = boards();
    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView {});

    CHECK(kindsOf(rows)
          == std::vector { Workspace, Workspace, Workspace, Workspace, Daemon, NewWorkspace, NewDaemon });
    CHECK(titlesOf(rows)
          == std::vector<std::string> {
              "contour", "api-gateway", "docs", "scratch", "home", "New Workspace", "New Daemon…" });
    CHECK(rows[0].groupTitle == "Workspaces on work");
    CHECK(rows[0].group == SwitcherGroup::WindowDaemon);
    CHECK(rows[1].groupTitle.empty());
    CHECK(rows[4].groupTitle == "Other daemons");
    CHECK(rows[4].group == SwitcherGroup::OtherDaemons);
    CHECK(rows[5].groupTitle == "Actions");
    CHECK(rows[6].groupTitle.empty());

    CHECK(rows[0].pill == SwitcherPill::Current);
    CHECK(rows[0].pillText == "Current");
    CHECK(rows[1].pill == SwitcherPill::None);
    CHECK(rows[2].pill == SwitcherPill::OtherWindow);
    CHECK(rows[2].pillText == "Other window");

    CHECK(rows[1].target == label("work", daemons[0].workspaces[1].ref));
    CHECK(rows[4].target == label("home", contour::session::AnyWorkspace {}));
    CHECK(rows[5].target == label("work", contour::session::NewWorkspace {}));
    CHECK(rows[4].daemon == 1);
}

TEST_CASE("a daemon row rolls up its listed workspaces' status", "[switcher]")
{
    auto const daemons = boards();
    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView {});
    REQUIRE(rows.size() == 7);
    CHECK(rows[4].status.glyph == TabBadgeKind::Error);
    CHECK(rows[4].status.urgency == Urgency::Failed);
    CHECK(contour::window::daemonStatusOf(daemons[0]) == SwitcherStatus {});
}

TEST_CASE("typing lists matching workspaces across every listed daemon, best first, with their daemon",
          "[switcher]")
{
    auto const daemons = boards();
    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView { .filter = "api" });

    CHECK(kindsOf(rows) == std::vector { Workspace, Workspace, Workspace, NewWorkspace });
    CHECK(titlesOf(rows)
          == std::vector<std::string> { "api-gateway", "api-docs", "rapid-proto", "New Workspace “api”" });
    CHECK(rows[0].group == SwitcherGroup::Matches);
    CHECK(rows[0].groupTitle == "Workspaces");
    CHECK(rows[3].groupTitle == "Actions");
    for (auto const& row: rows | std::views::take(3))
        CHECK(row.pill == SwitcherPill::Daemon);
    CHECK(rows[0].pillText == "work");
    CHECK(rows[1].pillText == "home");
    CHECK(rows[2].pillText == "home");
    CHECK(rows[0].titleMatches == std::vector { 0, 1, 2 });
    CHECK(rows[1].daemon == 1);
    CHECK(rows[3].target == label("work", contour::session::NewWorkspace {}));
}

TEST_CASE("no match leaves the create action alone", "[switcher]")
{
    auto const daemons = boards();
    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView { .filter = "zzz" });
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == NewWorkspace);
    CHECK(rows[0].title == "New Workspace “zzz”");
    CHECK(rows[0].groupTitle == "Actions");
}

TEST_CASE("a saved host is matched by name and never by its unlisted workspaces", "[switcher]")
{
    auto daemons = boards();
    auto ci = daemon(host("ci-box"), { workspace(1, "ci-runner") });
    ci.tag = "SSH";
    daemons.push_back(std::move(ci));

    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView { .filter = "ci" });
    CHECK(kindsOf(rows) == std::vector { Daemon, NewWorkspace });
    CHECK(rows[0].title == "ci-box");
    CHECK(rows[0].group == SwitcherGroup::OtherDaemons);
    CHECK(rows[0].groupTitle == "Other daemons");
    CHECK(rows[0].titleMatches == std::vector { 0, 1 });
    CHECK(rows[0].pill == SwitcherPill::Transport);
    CHECK(rows[0].pillText == "SSH");
}

TEST_CASE("while discovery runs, other daemons show one looking row", "[switcher]")
{
    auto daemons = boards();
    daemons.pop_back();
    auto const rows = switcherRows(
        SwitcherInputs { .daemons = daemons, .discovery = SwitcherDiscovery::Running }, SwitcherView {});
    CHECK(
        kindsOf(rows)
        == std::vector { Workspace, Workspace, Workspace, Workspace, Discovering, NewWorkspace, NewDaemon });
    CHECK(rows[4].title == "Looking for other daemons…");
    CHECK(rows[4].groupTitle == "Other daemons");

    SECTION("settled, no other daemon leaves the group out")
    {
        auto const settled = switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView {});
        CHECK(kindsOf(settled)
              == std::vector { Workspace, Workspace, Workspace, Workspace, NewWorkspace, NewDaemon });
    }
}

TEST_CASE("the daemon level lists that daemon's workspaces and its new workspace", "[switcher]")
{
    auto daemons = boards();
    auto const view = SwitcherView { .level = SwitcherLevel::Daemon, .daemon = 1 };
    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, view);

    CHECK(kindsOf(rows) == std::vector { Workspace, Workspace, Workspace, NewWorkspace });
    CHECK(titlesOf(rows)
          == std::vector<std::string> { "api-docs", "rapid-proto", "dotfiles", "New Workspace" });
    CHECK(rows[0].group == SwitcherGroup::DaemonWorkspaces);
    CHECK(rows[0].groupTitle == "Workspaces on home");
    CHECK(rows[3].groupTitle == "Actions");
    CHECK(rows[3].target == label("home", contour::session::NewWorkspace {}));
    CHECK(rows[0].status.glyph == TabBadgeKind::Error);

    SECTION("a filter keeps its matching workspaces, best first")
    {
        auto filtered = view;
        filtered.filter = "dotf";
        auto const matching = switcherRows(SwitcherInputs { .daemons = daemons }, filtered);
        CHECK(titlesOf(matching) == std::vector<std::string> { "dotfiles", "New Workspace “dotf”" });
        CHECK(matching[0].groupTitle == "Workspaces on home");
        CHECK(matching[0].titleMatches == std::vector { 0, 1, 2, 3 });
    }

    SECTION("a host still being listed shows one connecting row")
    {
        daemons.push_back(SwitcherDaemon { .address = host("ci-box"),
                                           .workspaceCount = std::nullopt,
                                           .tag = "SSH",
                                           .listing = SwitcherListing::Listing,
                                           .failure = {},
                                           .workspaces = {} });
        auto const connecting = switcherRows(SwitcherInputs { .daemons = daemons },
                                             SwitcherView { .level = SwitcherLevel::Daemon, .daemon = 2 });
        CHECK(kindsOf(connecting) == std::vector { Connecting, NewWorkspace });
        CHECK(connecting[0].title == "Connecting to ci-box…");
        CHECK(connecting[0].groupTitle == "Workspaces on ci-box");
    }
}

TEST_CASE("a window that reaches no other daemon lists its own workspaces and no other daemon", "[switcher]")
{
    auto const daemons = boards();
    auto const rows =
        switcherRows(SwitcherInputs { .daemons = daemons },
                     SwitcherView { .origin = SwitcherOrigin::Chip, .scope = SwitcherScope::WindowOnly });
    CHECK(kindsOf(rows) == std::vector { Workspace, Workspace, Workspace, Workspace, NewWorkspace });
}

TEST_CASE("the chip's rows are the top level: the window's daemon first, named local for the default label",
          "[switcher]")
{
    auto daemons = boards();
    daemons[0].address = label("default");
    auto const chip =
        switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView { .origin = SwitcherOrigin::Chip });

    CHECK(chip[0].groupTitle == "Workspaces on local");
    CHECK(chip[0].title == "contour");
    CHECK(kindsOf(chip) == kindsOf(switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView {})));
    CHECK(std::ranges::none_of(chip, [](SwitcherRow const& row) { return row.title == "All daemons…"; }));
}

TEST_CASE("switcherDaemonName names the default daemon local and keeps a host's saved name", "[switcher]")
{
    using contour::window::switcherDaemonName;
    CHECK(switcherDaemonName(label("default")) == "local");
    CHECK(switcherDaemonName(label("work")) == "work");
    CHECK(switcherDaemonName(host("ci-box")) == "ci-box");
    CHECK(switcherDaemonName(SwitchTarget { .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = {} })
          == "this daemon");
}

TEST_CASE("the chip opens every daemon, and a window without a daemon its own workspaces alone", "[switcher]")
{
    using contour::window::ChipSwitcherScope;
    using contour::window::switcherScopeFor;
    using contour::window::SwitcherWindowKind;
    STATIC_CHECK(ChipSwitcherScope == SwitcherScope::Everything);
    CHECK(switcherScopeFor(ChipSwitcherScope, SwitcherWindowKind::Daemon) == SwitcherScope::Everything);
    CHECK(switcherScopeFor(ChipSwitcherScope, SwitcherWindowKind::InProcess) == SwitcherScope::WindowOnly);
    CHECK(switcherScopeFor(SwitcherScope::OneDaemon, SwitcherWindowKind::Daemon) == SwitcherScope::OneDaemon);
}

TEST_CASE("a pick's one daemon, centred, lists its workspaces and no way out to every daemon", "[switcher]")
{
    auto const daemons = boards();
    auto const pick = switcherRows(SwitcherInputs { .daemons = daemons },
                                   SwitcherView { .daemon = 1, .scope = SwitcherScope::OneDaemon });
    CHECK(kindsOf(pick) == std::vector { Workspace, Workspace, Workspace, NewWorkspace });
    CHECK(pick[0].groupTitle == "Workspaces on home");
}

TEST_CASE("the new daemon level offers one action for a label it can act on", "[switcher]")
{
    auto const daemons = boards();
    auto const rowsFor = [&](std::string_view typed) {
        return switcherRows(SwitcherInputs { .daemons = daemons },
                            SwitcherView { .level = SwitcherLevel::NewDaemon, .filter = typed });
    };

    CHECK(rowsFor("").empty());
    CHECK(rowsFor("a b").empty());

    auto const fresh = rowsFor("research");
    REQUIRE(fresh.size() == 1);
    CHECK(fresh[0].kind == LabelAction);
    CHECK(fresh[0].title == "Start daemon “research” and switch this window to it");
    CHECK(fresh[0].target == label("research", contour::session::AnyWorkspace {}));

    auto const running = rowsFor("home");
    REQUIRE(running.size() == 1);
    CHECK(running[0].title == "Switch this window to daemon “home”");
    CHECK(running[0].target == label("home", contour::session::AnyWorkspace {}));
}

TEST_CASE("switcherInitialSelection starts on the first workspace this window does not show", "[switcher]")
{
    auto const daemons = boards();
    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView {});
    CHECK(switcherInitialSelection(rows) == 1);

    SECTION("with no such workspace it is the first row")
    {
        auto const none =
            switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView { .filter = "zzz" });
        CHECK(switcherInitialSelection(none) == 0);
        CHECK(switcherInitialSelection({}) == 0);
    }
}

TEST_CASE("switcherSelection keeps the row it was on, by its kind and target", "[switcher]")
{
    auto const daemons = boards();
    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView {});
    auto const identityOf = [&](std::size_t at) {
        return contour::window::switcherRowIdentity(rows[at]);
    };
    CHECK(switcherSelection(rows, identityOf(4)) == 4);
    CHECK(switcherSelection(rows,
                            contour::window::SwitcherRowIdentity {
                                .kind = Daemon, .target = label("gone", contour::session::AnyWorkspace {}) })
          == 0);
    CHECK(switcherSelection(rows, std::nullopt) == 0);

    SECTION("rows without a target are told apart by their kind, the looking row among them")
    {
        auto const looking = switcherRows(
            SwitcherInputs { .daemons = daemons, .discovery = SwitcherDiscovery::Running }, SwitcherView {});
        REQUIRE(
            kindsOf(looking)
            == std::vector {
                Workspace, Workspace, Workspace, Workspace, Daemon, Discovering, NewWorkspace, NewDaemon });
        auto const newDaemon = contour::window::switcherRowIdentity(looking[7]);
        CHECK(switcherSelection(looking, newDaemon) == 7);
        CHECK(switcherSelection(looking, contour::window::switcherRowIdentity(looking[5])) == 5);
        // Discovery answers: the looking row goes, and New Daemon… stays selected where it now is.
        CHECK(switcherSelection(rows, newDaemon) == 6);
    }
}

TEST_CASE("no daemon at all lists New Daemon… alone", "[switcher]")
{
    CHECK(kindsOf(switcherRows(SwitcherInputs {}, SwitcherView {})) == std::vector { NewDaemon });
    auto const looking =
        switcherRows(SwitcherInputs { .discovery = SwitcherDiscovery::Running }, SwitcherView {});
    CHECK(kindsOf(looking) == std::vector { Discovering, NewDaemon });
}

TEST_CASE("a view of a daemon that is not there lists nothing of it", "[switcher]")
{
    auto const daemons = boards();
    CHECK(switcherRows(SwitcherInputs { .daemons = daemons },
                       SwitcherView { .level = SwitcherLevel::Daemon, .daemon = 9 })
              .empty());
    CHECK(switcherRows(SwitcherInputs { .daemons = daemons },
                       SwitcherView { .daemon = 9, .scope = SwitcherScope::OneDaemon })
              .empty());
}

TEST_CASE("the daemon level says why a daemon could not be listed, and keeps what a refresh lists",
          "[switcher]")
{
    auto daemons = boards();
    auto const view = SwitcherView { .level = SwitcherLevel::Daemon, .daemon = 1 };

    SECTION("failed: one failure row, no stale workspaces")
    {
        daemons[1].listing = SwitcherListing::Failed;
        daemons[1].failure = "connection refused";
        auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, view);
        CHECK(kindsOf(rows) == std::vector { Failure, NewWorkspace });
        CHECK(rows[0].title == "Could not list home: connection refused");
        CHECK(rows[0].groupTitle == "Workspaces on home");
    }
    SECTION("listed again: the known workspaces stay while the answer comes")
    {
        daemons[1].listing = SwitcherListing::Listing;
        auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, view);
        CHECK(kindsOf(rows) == std::vector { Workspace, Workspace, Workspace, NewWorkspace });
    }
    SECTION("listed for the first time: one connecting row")
    {
        daemons[1].listing = SwitcherListing::Listing;
        daemons[1].workspaces.clear();
        auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, view);
        CHECK(kindsOf(rows) == std::vector { Connecting, NewWorkspace });
    }
}

TEST_CASE("the looking row is left out while typing", "[switcher]")
{
    auto const daemons = boards();
    auto const rows =
        switcherRows(SwitcherInputs { .daemons = daemons, .discovery = SwitcherDiscovery::Running },
                     SwitcherView { .filter = "api" });
    CHECK(std::ranges::none_of(rows, [](SwitcherRow const& row) { return row.kind == Discovering; }));
}

TEST_CASE("a window on a saved host still finds that host's own workspaces by typing", "[switcher]")
{
    auto daemons = boards();
    daemons[0].address = host("ci-box");
    daemons[0].tag = "SSH";
    auto const rows = switcherRows(SwitcherInputs { .daemons = daemons }, SwitcherView { .filter = "scr" });
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].title == "scratch");
    CHECK(rows[0].pillText == "ci-box");
    CHECK(rows[0].target
          == SwitchTarget { .kind = SwitchDaemonKind::Host,
                            .daemon = "ci-box",
                            .workspace = daemons[0].workspaces[3].ref });
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
