// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/SelectAnswer.hpp>
#include <contour/remote/SwitchPlan.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
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
        .visibility = vthost::WorkspaceVisibility::Hidden,
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
        return to->workspace == std::get<SwitchTo>(b).workspace;
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
        { "a workspace another window selected is switched to like any other",
          "1",
          { row(11, 1), row(13, 2) },
          13,
          SameDaemon::Yes,
          SwitchTo { .workspace = 11 } },
        { "a hidden workspace is switched to like any other",
          "2",
          { row(11, 1), hidden(row(13, 2)) },
          11,
          SameDaemon::Yes,
          SwitchTo { .workspace = 13 } },
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
    rows[1].visibility = WorkspaceVisibility::Hidden; // the daemon has not counted that window yet
    rows[2].visibility = WorkspaceVisibility::Shown;
    rows[3].visibility = WorkspaceVisibility::Hidden;
    rows[4].tabs = 3;

    auto const listed = contour::remote::switcherWorkspacesOf(rows, PopupWindow);

    REQUIRE(listed.size() == rows.size());
    CHECK(listed[0].state == SwitcherWorkspaceState::ThisWindow);
    CHECK(listed[1].state == SwitcherWorkspaceState::ShownElsewhere);
    CHECK(listed[2].state == SwitcherWorkspaceState::ShownElsewhere);
    CHECK(listed[3].state == SwitcherWorkspaceState::Hidden);
    CHECK(listed[4].state == SwitcherWorkspaceState::Hidden); // the daemon tells no workspace "left" any more
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

// A daemon no client attached to yet lists its own workspace, holding no tab: a window switched to it was
// emptied, and closed -- the last window, so the application quit.
TEST_CASE("workspaceRowsOf leaves out a workspace holding no tab", "[switch][plan]")
{
    auto empty = vtcontrol::WorkspaceInfo {};
    empty.id = "workspace:00000000-0000-0000-0000-000000000001";
    empty.position = 1;
    empty.native = "1";
    empty.visibility = "left";
    auto held = empty;
    held.id = "workspace:00000000-0000-0000-0000-000000000002";
    held.position = 2;
    held.native = "2";
    held.tabs = { vtcontrol::TabInfo {} };
    auto const none = [](uint64_t) {
        return std::optional<vtworkspace::WorkspaceId> {};
    };

    auto const rows = contour::remote::workspaceRowsOf(std::vector { empty, held }, none);

    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == 2);
    // So a daemon of none but its own empty workspace is switched to in a new workspace of its own.
    CHECK(sameDecision(
        contour::remote::planSwitch(naming({}),
                                    contour::remote::workspaceRowsOf(std::vector { empty }, none),
                                    std::nullopt,
                                    SameDaemon::No),
        SwitchTo { .workspace = 0 }));
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

TEST_CASE("a same-daemon switch naming a control id resolves from the directory without a listing",
          "[switch][plan]")
{
    auto const tabbed = [](uint64_t id, std::array<uint8_t, 16> controlId) {
        return contour::window::DirectoryWorkspace { .id = id,
                                                     .label = {},
                                                     .tabs = { contour::window::DirectoryTab {} },
                                                     .controlId = controlId,
                                                     .git = {} };
    };
    auto const idOf = [](uint8_t last) {
        auto id = std::array<uint8_t, 16> {};
        id.back() = last;
        id.at(14) = 0xab; // so the id's end is hex, not a position
        id.front() = 0x5e;
        return id;
    };
    auto const stated = contour::window::WorkspaceDirectory {
        .workspaces = { tabbed(1, idOf(1)), tabbed(2, idOf(2)) },
        .selected = 1,
    };
    auto const idText = vtcontrol::toString(vtcontrol::ObjectRef {
        .kind = vtcontrol::ObjectKind::Workspace, .uuid = vtcontrol::Uuid { .bytes = idOf(2) } });
    auto const byId = contour::session::workspaceTargetOf(idText);
    REQUIRE(contour::remote::namesControlId(byId));
    auto const none = [](uint64_t) {
        return std::optional<vtworkspace::WorkspaceId> {};
    };

    CHECK(contour::remote::resolvesFromDirectory(byId, stated));
    auto const rows = contour::remote::workspaceRowsOf(stated, none);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].ref.uuid.bytes == idOf(2));
    CHECK(sameDecision(contour::remote::planSwitch(naming(idText), rows, 1, SameDaemon::Yes),
                       SwitchTo { .workspace = 2 }));

    // A daemon that states no control ids leaves them to its listing; what names none needs no listing.
    auto unstated = stated;
    unstated.workspaces[1].controlId = {};
    CHECK_FALSE(contour::remote::resolvesFromDirectory(byId, unstated));
    CHECK(contour::remote::resolvesFromDirectory(contour::session::WorkspaceAt { .position = 2 }, unstated));
    // A workspace holding no tab is no row: its id is the listing's to resolve.
    auto emptied = stated;
    emptied.workspaces[1].tabs.clear();
    CHECK_FALSE(contour::remote::resolvesFromDirectory(byId, emptied));
    // The end of an id the directory carries resolves there too.
    CHECK(contour::remote::resolvesFromDirectory(
        contour::session::workspaceTargetOf(idText.substr(idText.size() - 6)), stated));
}

TEST_CASE("a switch naming a control id the directory does not carry goes to the listing", "[switch][plan]")
{
    auto absentId = std::array<uint8_t, 16> {};
    absentId.front() = 0x5e;
    absentId.back() = 9;
    auto const named = contour::session::workspaceTargetOf(vtcontrol::toString(vtcontrol::ObjectRef {
        .kind = vtcontrol::ObjectKind::Workspace, .uuid = vtcontrol::Uuid { .bytes = absentId } }));
    auto const asRef = contour::session::WorkspaceTarget { vtcontrol::ObjectRef {
        .kind = vtcontrol::ObjectKind::Workspace, .uuid = vtcontrol::Uuid { .bytes = absentId } } };

    SECTION("an empty directory, as a reconnect holds before its first batch")
    {
        auto const empty = contour::window::WorkspaceDirectory {};
        CHECK_FALSE(contour::remote::resolvesFromDirectory(named, empty));
        CHECK_FALSE(contour::remote::resolvesFromDirectory(asRef, empty));
    }
    SECTION("an id absent from it, as a workspace created before its batch settled")
    {
        auto known = std::array<uint8_t, 16> {};
        known.back() = 1;
        auto const directory = contour::window::WorkspaceDirectory {
            .workspaces = { contour::window::DirectoryWorkspace {
                .id = 1,
                .label = {},
                .tabs = { contour::window::DirectoryTab {} },
                .controlId = known,
                .git = {} } },
            .selected = 1,
        };
        CHECK_FALSE(contour::remote::resolvesFromDirectory(named, directory));
        CHECK_FALSE(contour::remote::resolvesFromDirectory(asRef, directory));
    }
}

TEST_CASE("a switch is judged by the answer to its own selection request", "[switch][select]")
{
    using contour::remote::SelectAnswer;
    using contour::remote::switchRefusal;
    constexpr auto A = uint64_t { 1 };
    constexpr auto B = uint64_t { 2 };
    auto const before = std::set<uint64_t> { A, B };

    // Switches to B (1), back to A (2), and to B again (3): the daemon answered the first two in one batch,
    // stating A. The answer to request 1 asked for B and states A, but request 3 is the switch waiting.
    auto const merged = std::array { SelectAnswer { .request = 1, .asked = B, .selected = A },
                                     SelectAnswer { .request = 2, .asked = A, .selected = A } };
    CHECK_FALSE(switchRefusal(3, B, before, merged).has_value());

    // Its own answer decides.
    auto const taken = std::array { SelectAnswer { .request = 3, .asked = B, .selected = B } };
    CHECK_FALSE(switchRefusal(3, B, before, taken).has_value());
    auto const unknown = std::array { SelectAnswer { .request = 3, .asked = 9, .selected = A } };
    CHECK(switchRefusal(3, 9, before, unknown)
          == std::optional<std::string_view> { "the daemon has no such workspace" });

    // A new workspace is one not known before; still one known means none was made.
    auto const made = std::array { SelectAnswer { .request = 4, .asked = 0, .selected = 7 } };
    CHECK_FALSE(switchRefusal(4, 0, before, made).has_value());
    auto const notMade = std::array { SelectAnswer { .request = 4, .asked = 0, .selected = A } };
    CHECK(switchRefusal(4, 0, before, notMade)
          == std::optional<std::string_view> { "the daemon could not make a workspace" });
}

TEST_CASE(
    "launchDaemonAddress names a launch's daemon as discovery does, by its socket in the label directory",
    "[switch][plan]")
{
    using contour::remote::DaemonAddress;
    using contour::remote::launchDaemonAddress;
    using vthost::proto::SwitchDaemonKind;
    auto const dir = std::filesystem::path { "/run/user/1000/contour" };
    auto const labelled = [](std::string name) {
        return DaemonAddress { .kind = SwitchDaemonKind::Label, .name = std::move(name) };
    };

    SECTION("--socket outside the label directory is no label's daemon")
    {
        auto const address = launchDaemonAddress("/tmp/elsewhere/own.sock", dir);
        CHECK(address.kind == SwitchDaemonKind::Same);
        CHECK(address != labelled("default"));
    }
    SECTION("the default socket is default")
    {
        CHECK(launchDaemonAddress(dir / "default", dir) == labelled("default"));
    }
    SECTION("$CONTOUR_MUX or --socket at another label's socket is that label")
    {
        CHECK(launchDaemonAddress(dir / "work", dir) == labelled("work"));
    }
    SECTION("a path that is not normal still names its label")
    {
        CHECK(launchDaemonAddress(dir / "." / "work", dir) == labelled("work"));
    }
}
