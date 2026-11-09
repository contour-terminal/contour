// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/SwitchPlan.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using contour::remote::SameDaemon;
using contour::remote::SwitchDecision;
using contour::remote::SwitchFocus;
using contour::remote::SwitchNoop;
using contour::remote::SwitchPick;
using contour::remote::SwitchTo;
using contour::remote::SwitchUnresolved;
using contour::remote::WorkspaceRow;

namespace
{

/// The window of this process the tests name as the one showing a workspace.
constexpr auto OtherWindow = vtworkspace::WorkspaceId { 7 };

/// @return A workspace row of native id @p id at @p position, whose uuid ends in @p id.
[[nodiscard]] WorkspaceRow row(uint64_t id,
                               uint32_t position,
                               std::string const& label = {},
                               std::optional<vtworkspace::WorkspaceId> shownHere = std::nullopt)
{
    auto uuid = vtcontrol::Uuid {};
    uuid.bytes.back() = static_cast<uint8_t>(id);
    return WorkspaceRow {
        .id = id,
        .ref = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace, .uuid = uuid },
        .label = label,
        .position = position,
        .name = label,
        .visibility = vthost::WorkspaceVisibility::Left,
        .tabs = 1,
        .shownByWindowHere = shownHere,
    };
}

/// @return @p workspace, as one a window switched away from.
[[nodiscard]] WorkspaceRow hidden(WorkspaceRow workspace)
{
    workspace.visibility = vthost::WorkspaceVisibility::Hidden;
    return workspace;
}

/// A daemon of three workspaces: 11 labelled "ops", 12 shown by another window here, 13 unlabelled.
[[nodiscard]] std::vector<WorkspaceRow> threeWorkspaces()
{
    return { row(11, 1, "ops"), row(12, 2, "build", OtherWindow), row(13, 3) };
}

/// @return A target on the same daemon naming @p workspace.
[[nodiscard]] contour::session::SwitchTarget naming(std::string_view workspace)
{
    return contour::session::SwitchTarget { .kind = vthost::proto::SwitchDaemonKind::Same,
                                            .daemon = {},
                                            .workspace = contour::session::workspaceTargetOf(workspace) };
}

/// One row of the planSwitch table.
struct PlanCase
{
    std::string_view name;
    std::string workspace;           ///< The target's workspace spec.
    std::vector<WorkspaceRow> rows;  ///< The daemon's workspaces.
    std::optional<uint64_t> current; ///< What the asking window shows.
    SameDaemon same;                 ///< Whether the daemon is the window's own.
    SwitchDecision expected;         ///< What the switch does.
};

[[nodiscard]] bool sameDecision(SwitchDecision const& a, SwitchDecision const& b)
{
    if (a.index() != b.index())
        return false;
    if (auto const* focus = std::get_if<SwitchFocus>(&a))
        return focus->window == std::get<SwitchFocus>(b).window;
    if (auto const* to = std::get_if<SwitchTo>(&a))
        return to->workspace == std::get<SwitchTo>(b).workspace && to->undo == std::get<SwitchTo>(b).undo;
    return true;
}

} // namespace

TEST_CASE("planSwitch resolves every target as the spec says", "[switch][plan]")
{
    auto const cases = std::to_array<PlanCase>({
        { "a named workspace is that one, by label",
          "ops",
          threeWorkspaces(),
          std::nullopt,
          SameDaemon::No,
          SwitchTo { .workspace = 11 } },
        { "a named workspace is that one, by position",
          "3",
          threeWorkspaces(),
          std::nullopt,
          SameDaemon::No,
          SwitchTo { .workspace = 13 } },
        { "a named workspace is that one, by its full id",
          vtcontrol::toString(threeWorkspaces()[2].ref),
          threeWorkspaces(),
          std::nullopt,
          SameDaemon::No,
          SwitchTo { .workspace = 13 } },
        { "a hidden or left workspace is switched to like any other",
          "1",
          { row(11, 1), row(13, 2) },
          13,
          SameDaemon::Yes,
          SwitchTo { .workspace = 11 } },
        { "a hidden workspace a switch does not land on is let go of as hidden again",
          "2",
          { row(11, 1), hidden(row(13, 2)) },
          11,
          SameDaemon::Yes,
          SwitchTo { .workspace = 13, .undo = vthost::proto::WorkspaceRelease::SwitchedAway } },
        { "several workspaces and none named: the user picks",
          "",
          threeWorkspaces(),
          std::nullopt,
          SameDaemon::No,
          SwitchPick {} },
        { "exactly one and none named: that one",
          "",
          { row(21, 1) },
          std::nullopt,
          SameDaemon::No,
          SwitchTo { .workspace = 21 } },
        { "none at all: a new workspace", "", {}, std::nullopt, SameDaemon::No, SwitchTo { .workspace = 0 } },
        { "the new-workspace spec: a new one, though the daemon holds several",
          std::string { contour::remote::NewWorkspaceSpec },
          threeWorkspaces(),
          11,
          SameDaemon::Yes,
          SwitchTo { .workspace = 0 } },
        { "shown by another window of this process: focus it",
          "build",
          threeWorkspaces(),
          11,
          SameDaemon::Yes,
          SwitchFocus { .window = OtherWindow } },
        { "the only workspace, shown by another window here: focus it",
          "",
          { row(12, 1, "build", OtherWindow) },
          std::nullopt,
          SameDaemon::No,
          SwitchFocus { .window = OtherWindow } },
        { "this window's own workspace: nothing to do",
          "ops",
          threeWorkspaces(),
          11,
          SameDaemon::Yes,
          SwitchNoop {} },
        { "the same native id on another daemon is not this window's",
          "ops",
          threeWorkspaces(),
          11,
          SameDaemon::No,
          SwitchTo { .workspace = 11 } },
        { "a spec naming nothing is unresolved",
          "nosuch",
          threeWorkspaces(),
          std::nullopt,
          SameDaemon::No,
          SwitchUnresolved { .reason = {} } },
        { "a position past the end is unresolved",
          "9",
          threeWorkspaces(),
          std::nullopt,
          SameDaemon::No,
          SwitchUnresolved { .reason = {} } },
    });
    for (auto const& each: cases)
    {
        INFO(each.name);
        auto const decision =
            contour::remote::planSwitch(naming(each.workspace), each.rows, each.current, each.same);
        CHECK(sameDecision(decision, each.expected));
    }
}

TEST_CASE("planSwitch says why a target names no workspace", "[switch][plan]")
{
    auto const rows = threeWorkspaces();
    auto const decision = contour::remote::planSwitch(naming("nosuch"), rows, std::nullopt, SameDaemon::No);
    REQUIRE(std::holds_alternative<SwitchUnresolved>(decision));
    CHECK(std::get<SwitchUnresolved>(decision).reason.contains("nosuch"));
}

TEST_CASE("workspaceDisplayName: label, else active tab title, else workspace N", "[switch][plan]")
{
    struct NameCase
    {
        std::string_view label;
        std::string_view title;
        uint32_t position;
        std::string_view expected;
    };
    auto const cases = std::to_array<NameCase>({
        { "ops", "vim", 1, "ops" },
        { "", "vim", 2, "vim" },
        { "", "", 3, "workspace 3" },
        { "ops", "", 4, "ops" },
    });
    for (auto const& [label, title, position, expected]: cases)
    {
        INFO(expected);
        CHECK(contour::remote::workspaceDisplayName(label, title, position) == expected);
    }
}

TEST_CASE("switcherWorkspacesOf marks where each workspace is shown, as the popup's window sees it",
          "[switch][plan][switcher]")
{
    using contour::window::SwitcherWorkspaceState;
    using vthost::WorkspaceVisibility;
    constexpr auto PopupWindow = vtworkspace::WorkspaceId { 3 };
    auto rows = std::vector<WorkspaceRow> {
        row(11, 1, "mine", PopupWindow),
        row(12, 2, "other", OtherWindow),
        row(13, 3, "remote"),
        row(14, 4, "away"),
        row(15, 5, "left"),
    };
    rows[0].visibility = WorkspaceVisibility::Shown;
    rows[1].visibility = WorkspaceVisibility::Left; // the daemon has not counted that window yet
    rows[2].visibility = WorkspaceVisibility::Shown;
    rows[3].visibility = WorkspaceVisibility::Hidden;
    rows[4].tabs = 3;

    auto const listed = contour::remote::switcherWorkspacesOf(rows, PopupWindow);

    REQUIRE(listed.size() == rows.size());
    CHECK(listed[0].state == SwitcherWorkspaceState::ThisWindow);
    CHECK(listed[1].state == SwitcherWorkspaceState::ShownElsewhere);
    CHECK(listed[2].state == SwitcherWorkspaceState::ShownElsewhere);
    CHECK(listed[3].state == SwitcherWorkspaceState::Hidden);
    CHECK(listed[4].state == SwitcherWorkspaceState::Left);
    CHECK(listed[4].tabs == 3);
    CHECK(listed[4].position == 5);
    CHECK(listed[4].name == "left");
    CHECK(listed[0].ref == rows[0].ref);
}

TEST_CASE("workspaceRowsOf counts each workspace's tabs", "[switch][plan]")
{
    auto info = vtcontrol::WorkspaceInfo {};
    info.id = "workspace:00000000-0000-0000-0000-000000000001";
    info.position = 1;
    info.native = "9";
    info.visibility = "hidden";
    info.tabs = { vtcontrol::TabInfo {}, vtcontrol::TabInfo {} };
    auto const infos = std::vector { info };

    auto const rows = contour::remote::workspaceRowsOf(
        infos, [](uint64_t) { return std::optional<vtworkspace::WorkspaceId> {}; });

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].tabs == 2);
    CHECK(rows[0].visibility == vthost::WorkspaceVisibility::Hidden);
}

TEST_CASE("a switch that does not land leaves its target as it was", "[switch][plan]")
{
    using vthost::WorkspaceVisibility;
    using vthost::proto::WorkspaceRelease;
    STATIC_CHECK(contour::remote::undoReleaseFor(WorkspaceVisibility::Hidden)
                 == WorkspaceRelease::SwitchedAway);
    STATIC_CHECK(contour::remote::undoReleaseFor(WorkspaceVisibility::Left) == WorkspaceRelease::Closed);
    STATIC_CHECK(contour::remote::undoReleaseFor(WorkspaceVisibility::Shown) == WorkspaceRelease::Closed);
}

// Review (PR 3, item 13): the switching actions sent a position as text, and the daemon's vocabulary matches
// a label first: a workspace labelled "2" won over the second workspace.
TEST_CASE("planSwitch takes a position or a control id for that alone, never for a label", "[switch][plan]")
{
    auto const rows = std::vector { row(11, 1, "2"), row(12, 2, "build"), row(13, 3) };
    auto const at = [](contour::session::WorkspaceTarget workspace) {
        return contour::session::SwitchTarget { .kind = vthost::proto::SwitchDaemonKind::Same,
                                                .daemon = {},
                                                .workspace = std::move(workspace) };
    };
    using contour::remote::planSwitch;
    CHECK(
        sameDecision(planSwitch(at(contour::session::WorkspaceAt { 2 }), rows, std::nullopt, SameDaemon::No),
                     SwitchTo { .workspace = 12 }));
    // Typed, "2" is what the control verbs make of it: the label.
    CHECK(sameDecision(planSwitch(naming("2"), rows, std::nullopt, SameDaemon::No),
                       SwitchTo { .workspace = 11 }));
    CHECK(sameDecision(planSwitch(at(rows[2].ref), rows, std::nullopt, SameDaemon::No),
                       SwitchTo { .workspace = 13 }));
    CHECK(std::holds_alternative<SwitchUnresolved>(
        planSwitch(at(contour::session::WorkspaceAt { 9 }), rows, std::nullopt, SameDaemon::No)));
}
