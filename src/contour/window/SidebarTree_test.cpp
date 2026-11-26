// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarTree.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace contour::window;
using namespace std::chrono_literals;
using vtbackend::BlockedKind;
using vtbackend::ProgramState;
using vtbackend::ProgramStatusRecord;

namespace
{
/// UTF-8 built from bytes, so that no source line holds a bidirectional character.
std::string utf8(std::initializer_list<unsigned char> bytes)
{
    return { bytes.begin(), bytes.end() };
}

/// U+202E, right-to-left override.
std::string bidiOverride()
{
    return utf8({ 0xE2, 0x80, 0xAE });
}

/// U+200B.
std::string zeroWidthSpace()
{
    return utf8({ 0xE2, 0x80, 0x8B });
}

constexpr auto Now = std::chrono::system_clock::time_point { std::chrono::seconds { 1'000'000 } };

ProgramStatusRecord recordOf(ProgramState state,
                             std::string message = {},
                             std::chrono::seconds age = 42s,
                             BlockedKind kind = BlockedKind::None,
                             std::optional<uint8_t> progress = std::nullopt)
{
    auto record = ProgramStatusRecord {};
    record.state = state;
    record.kind = kind;
    record.progress = progress;
    record.app = "app";
    record.message = std::move(message);
    record.since = Now - age;
    record.generation = 1;
    return record;
}

DirectoryPane paneOf(uint64_t key, std::string title = {}, std::vector<ProgramStatusRecord> records = {})
{
    auto pane = DirectoryPane {};
    pane.key = PaneKey { key };
    pane.title = std::move(title);
    pane.status = std::move(records);
    return pane;
}

DirectoryTab tabOf(uint64_t id, std::string name, std::vector<DirectoryPane> panes)
{
    auto tab = DirectoryTab {};
    tab.id = id;
    tab.name = std::move(name);
    tab.panes = std::move(panes);
    return tab;
}

DirectoryWorkspace workspaceOf(uint64_t id, std::string name, std::vector<DirectoryTab> tabs)
{
    auto workspace = DirectoryWorkspace {};
    workspace.id = id;
    workspace.name = std::move(name);
    workspace.tabs = std::move(tabs);
    return workspace;
}

WorkspaceDirectory directoryOf(std::vector<DirectoryWorkspace> workspaces)
{
    auto directory = WorkspaceDirectory {};
    directory.workspaces = std::move(workspaces);
    return directory;
}

std::vector<SidebarRow> rowsOf(WorkspaceDirectory const& directory,
                               SidebarState const& state = {},
                               std::optional<TabRef> displayed = std::nullopt)
{
    return sidebarRows(directory, TabAttention {}, state, Now, displayed);
}

/// A tab holding one pane that reports @p record.
WorkspaceDirectory oneTabWith(ProgramStatusRecord record)
{
    return directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { paneOf(1, "p", { std::move(record) }) }) }) });
}

/// A workspace of the window's one daemonless section.
WorkspaceKey workspaceKeyOf(uint64_t workspace)
{
    return WorkspaceKey { .daemon = {}, .instance = {}, .workspace = workspace };
}

/// A tab of the window's one daemonless section.
TabKey tabKeyOf(uint64_t workspace, uint64_t tab)
{
    return TabKey { .workspace = workspaceKeyOf(workspace), .tab = tab };
}

SidebarRow const& tabRowOf(std::vector<SidebarRow> const& rows)
{
    return rows.at(1);
}
} // namespace

TEST_CASE("SidebarTree.unsplit_tab_has_no_pane_rows", "[sidebar]")
{
    auto const rows = rowsOf(directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { paneOf(1) }) }) }));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].key.kind == SidebarRowKind::Workspace);
    CHECK(rows[1].key.kind == SidebarRowKind::Tab);
    CHECK(rows[1].expansion == RowExpansion::None);
}

TEST_CASE("SidebarTree.split_tab_lists_panes_depth_first", "[sidebar]")
{
    auto const rows = rowsOf(directoryOf(
        { workspaceOf(7, {}, { tabOf(3, "t", { paneOf(10, "a"), paneOf(11, "b") }), tabOf(4, "u", {}) }) }));
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].title == "Workspace 1");
    CHECK(rows[1].key == SidebarRowKey { .kind = SidebarRowKind::Tab, .workspace = 7, .tab = 3, .pane = {} });
    CHECK(rows[1].expansion == RowExpansion::Expanded);
    CHECK(
        rows[2].key
        == SidebarRowKey { .kind = SidebarRowKind::Pane, .workspace = 7, .tab = 3, .pane = PaneKey { 10 } });
    CHECK(rows[2].title == "a");
    CHECK(rows[2].depth == 2);
    CHECK(rows[3].title == "b");
    CHECK(rows[4].key.tab == 4);
    CHECK(rows[4].title == "u");
}

TEST_CASE("SidebarTree.collapsed_workspace_rolls_up_most_urgent_pane", "[sidebar]")
{
    struct Case
    {
        std::vector<ProgramState> states;
        Urgency urgency;
        TabBadgeKind glyph;
    };
    auto const cases = std::vector<Case> {
        { { ProgramState::Working, ProgramState::Blocked, ProgramState::Error },
          Urgency::NeedsYou,
          TabBadgeKind::Blocked },
        { { ProgramState::Working, ProgramState::Error, ProgramState::Done },
          Urgency::Failed,
          TabBadgeKind::Error },
        { { ProgramState::Working, ProgramState::Done }, Urgency::DoneUnseen, TabBadgeKind::Done },
        { { ProgramState::Idle, ProgramState::Working }, Urgency::Working, TabBadgeKind::Working },
        { { ProgramState::Idle }, Urgency::Idle, TabBadgeKind::None },
    };
    for (auto const& [states, urgency, glyph]: cases)
    {
        auto tabs = std::vector<DirectoryTab> {};
        for (auto const& [index, state]: core::views::enumerate(states))
            tabs.push_back(tabOf(static_cast<uint64_t>(index) + 1,
                                 "t",
                                 { paneOf(static_cast<uint64_t>(index) + 1, "p", { recordOf(state) }) }));
        auto const rows = rowsOf(directoryOf({ workspaceOf(1, "w", std::move(tabs)) }),
                                 SidebarState { .toggled = { workspaceKeyOf(1) } });
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].urgency == urgency);
        CHECK(rows[0].glyph == glyph);
        CHECK(rows[0].expansion == RowExpansion::Collapsed);
    }
}

TEST_CASE("SidebarTree.collapsed_workspace_hides_children", "[sidebar]")
{
    auto const directory = directoryOf(
        { workspaceOf(1, "w", { tabOf(1, "t", { paneOf(1), paneOf(2) }) }), workspaceOf(2, "x", {}) });
    CHECK(rowsOf(directory, SidebarState { .toggled = { workspaceKeyOf(1) } }).size() == 2);
    auto const tabFolded = rowsOf(directory, SidebarState { .collapsedTabs = { tabKeyOf(1, 1) } });
    REQUIRE(tabFolded.size() == 3);
    CHECK(tabFolded[1].expansion == RowExpansion::Collapsed);
    CHECK(rowsOf(directory).size() == 5);
}

TEST_CASE("SidebarTree.reason_text_is_sanitized_and_elided", "[sidebar]")
{
    struct Case
    {
        std::string message;
        std::string reason;
    };
    auto const cases = std::vector<Case> {
        { "plain", "plain" },
        { "a\x01"
          "b\x1b[31mc\x7f",
          "ab[31mc" },
        { "ab" + bidiOverride() + "cd" + zeroWidthSpace() + "ef",
          "abcdef" },                     // bidi override, zero-width space
        { "<b>bold</b>", "<b>bold</b>" }, // plain text: a view never renders it as markup
        { "line one\nline two", "line one line two" },
    };
    for (auto const& [message, reason]: cases)
        CHECK(tabRowOf(rowsOf(oneTabWith(recordOf(ProgramState::Working, message)))).reason == reason);

    auto const big = tabRowOf(rowsOf(oneTabWith(recordOf(ProgramState::Working, std::string(10'000, 'x')))));
    CHECK(big.reason == std::string(MaxRowTextLength - 1, 'x') + "…");
}

TEST_CASE("SidebarTree.full_reason_is_sanitized_whole_and_capped", "[sidebar]")
{
    // Longer than the row's line, but whole on the hover card, and sanitized the same way.
    auto const longReason = std::string(300, 'y');
    auto const row = tabRowOf(rowsOf(oneTabWith(recordOf(ProgramState::Blocked,
                                                         "a\x01" + bidiOverride() + longReason + "\nmore",
                                                         42s,
                                                         BlockedKind::Permission))));
    CHECK(row.reason.size() < row.fullReason.size());
    CHECK(row.fullReason == "Permission · a" + longReason + " more");

    // Even the full reason is bounded: a hostile one cannot flood the card.
    auto const flood =
        tabRowOf(rowsOf(oneTabWith(recordOf(ProgramState::Working, std::string(100'000, 'z')))));
    CHECK(flood.fullReason == std::string(MaxFullReasonLength - 1, 'z') + "…");

    // A finished command's verdict is short already: both say the same.
    auto pane = paneOf(1, "p");
    pane.finished = vthost::proto::FinishedCommand { .exitCode = 1 };
    pane.finishedSerial = 1;
    auto const failed = tabRowOf(rowsOf(directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { pane }) }) })));
    CHECK(failed.fullReason == failed.reason);
    CHECK(failed.reason == "exit 1");
}

TEST_CASE("SidebarTree.blocked_reason_is_prefixed_by_its_kind", "[sidebar]")
{
    struct Case
    {
        BlockedKind kind;
        std::string message;
        std::string reason;
    };
    auto const cases = std::vector<Case> {
        { BlockedKind::Permission, "Bash: ls", "Permission · Bash: ls" },
        { BlockedKind::Question, "Which one?", "Question · Which one?" },
        { BlockedKind::Auth, "Password", "Sign-in · Password" },
        { BlockedKind::Permission, "", "Permission" },
        { BlockedKind::None, "Waiting", "Waiting" },
    };
    for (auto const& [kind, message, reason]: cases)
    {
        auto const row = tabRowOf(rowsOf(oneTabWith(recordOf(ProgramState::Blocked, message, 42s, kind))));
        CHECK(row.reason == reason);
        CHECK(row.glyph == TabBadgeKind::Blocked);
    }
}

TEST_CASE("SidebarTree.a_permission_names_its_tool_in_the_label_and_its_command_apart", "[sidebar]")
{
    struct Case
    {
        BlockedKind kind;
        std::string message;
        std::string label;
        std::string command;
    };
    auto const cases = std::vector<Case> {
        { BlockedKind::Permission, "Bash: psql $DEV_URL", "Permission · Bash: ", "psql $DEV_URL" },
        { BlockedKind::Permission, "Edit: src/a.cpp", "Permission · Edit: ", "src/a.cpp" },
        { BlockedKind::Permission, "rm -rf build", "Permission · ", "rm -rf build" },
        { BlockedKind::Permission, "Run this: ls", "Permission · ", "Run this: ls" },
        { BlockedKind::Permission, "Bash: ", "Permission · ", "Bash: " },
        { BlockedKind::Permission, ": ls", "Permission · ", ": ls" },
        { BlockedKind::Question, "Pick: a or b", "Question · Pick: a or b", "" },
    };
    for (auto const& [kind, message, label, command]: cases)
    {
        INFO(message);
        auto const row = tabRowOf(rowsOf(oneTabWith(recordOf(ProgramState::Blocked, message, 42s, kind))));
        CHECK(row.reasonLabel == label);
        CHECK(row.reasonCommand == command);
        CHECK(row.reason == label + command);
    }
}

TEST_CASE("SidebarTree.failed_command_shows_exit_and_duration", "[sidebar]")
{
    struct Case
    {
        std::optional<int32_t> exitCode;
        std::optional<uint64_t> durationMs;
        std::string reason;
    };
    auto const cases = std::vector<Case> {
        { 1, 18'000, "exit 1 · 18s" },
        { 2, std::nullopt, "exit 2" },
        { 127, 850, "exit 127 · 850ms" },
    };
    for (auto const& [exitCode, durationMs, reason]: cases)
    {
        auto pane = paneOf(1, "p");
        pane.finished = vthost::proto::FinishedCommand { .exitCode = exitCode, .durationMs = durationMs };
        pane.finishedSerial = 1;
        auto const rows = rowsOf(directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { pane }) }) }));
        CHECK(tabRowOf(rows).reason == reason);
        CHECK(tabRowOf(rows).urgency == Urgency::Failed);
        CHECK(tabRowOf(rows).glyph == TabBadgeKind::Error);
    }
}

TEST_CASE("SidebarTree.a_failed_command_with_its_line_splits_into_words_and_command", "[sidebar]")
{
    auto pane = paneOf(1, "p");
    pane.finished = vthost::proto::FinishedCommand { .exitCode = 1,
                                                     .durationMs = std::nullopt,
                                                     .commandLine = "make deploy" };
    pane.finishedSerial = 1;
    auto const rows = rowsOf(directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { pane }) }) }));
    // The separator travels with the command: where a narrow picker drops the command, the words end clean.
    CHECK(tabRowOf(rows).reasonLabel == "exit 1");
    CHECK(tabRowOf(rows).reasonSeparator == " · ");
    CHECK(tabRowOf(rows).reasonCommand == "make deploy");
    CHECK(tabRowOf(rows).reason == "exit 1 · make deploy");
    CHECK(tabRowOf(rows).fullReasonLabel == "exit 1 · ");

    // No command, no separator.
    auto alone = paneOf(1, "p");
    alone.finished =
        vthost::proto::FinishedCommand { .exitCode = 1, .durationMs = std::nullopt, .commandLine = {} };
    alone.finishedSerial = 1;
    auto const bare = rowsOf(directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { alone }) }) }));
    CHECK(tabRowOf(bare).reasonLabel == "exit 1");
    CHECK(tabRowOf(bare).reasonSeparator.empty());

    // A plain reason is all words.
    auto const plain = rowsOf(oneTabWith(recordOf(ProgramState::Done, "All tests pass")));
    CHECK(tabRowOf(plain).reasonLabel == "All tests pass");
    CHECK(tabRowOf(plain).reasonSeparator.empty());
    CHECK(tabRowOf(plain).reasonCommand.empty());
}

TEST_CASE("SidebarTree.bell_is_needs_you_with_its_own_glyph", "[sidebar]")
{
    auto pane = paneOf(1, "p");
    pane.bell = vthost::proto::PaneBell::Rang;
    pane.bellSerial = 1;
    auto const rows = rowsOf(directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { pane }) }) }));
    CHECK(tabRowOf(rows).urgency == Urgency::NeedsYou);
    CHECK(tabRowOf(rows).glyph == TabBadgeKind::Bell);
    CHECK(rows[0].glyph == TabBadgeKind::Bell);
}

TEST_CASE("SidebarTree.live_time_formats", "[sidebar]")
{
    struct Case
    {
        std::chrono::seconds elapsed;
        std::string text;
    };
    auto const cases = std::vector<Case> {
        { -5s, "0:00" },    { 0s, "0:00" },       { 42s, "0:42" },        { 252s, "4:12" },
        { 3599s, "59:59" }, { 3723s, "1:02:03" }, { 36000s, "10:00:00" },
    };
    for (auto const& [elapsed, text]: cases)
        CHECK(formatLive(elapsed) == text);

    auto const row = tabRowOf(rowsOf(oneTabWith(recordOf(ProgramState::Working, "x", 252s))));
    CHECK(row.time == TimeText { .kind = TimeKind::Live, .text = "4:12", .percent = std::nullopt });
}

TEST_CASE("SidebarTree.relative_time_formats", "[sidebar]")
{
    struct Case
    {
        std::chrono::seconds elapsed;
        std::string text;
    };
    auto const cases = std::vector<Case> {
        { 0s, "now" },        { 9s, "now" },    { 10s, "10s ago" }, { 59s, "59s ago" }, { 120s, "2m ago" },
        { 3599s, "59m ago" }, { 3h, "3h ago" }, { 47h, "1d ago" },  { 48h, "2d ago" },
    };
    for (auto const& [elapsed, text]: cases)
        CHECK(formatRelative(elapsed) == text);

    auto const row = tabRowOf(rowsOf(oneTabWith(recordOf(ProgramState::Done, "All 214 pass", 120s))));
    CHECK(row.time == TimeText { .kind = TimeKind::Relative, .text = "2m ago", .percent = std::nullopt });
    CHECK(row.reason == "All 214 pass");
    CHECK(row.glyph == TabBadgeKind::Done);
}

TEST_CASE("SidebarTree.progress_wins_over_live_time", "[sidebar]")
{
    auto const row =
        tabRowOf(rowsOf(oneTabWith(recordOf(ProgramState::Working, "x", 42s, BlockedKind::None, 63))));
    CHECK(row.time == TimeText { .kind = TimeKind::Progress, .text = "63%", .percent = 63 });
}

TEST_CASE("SidebarTree.a_record_without_a_time_shows_none", "[sidebar]")
{
    auto record = recordOf(ProgramState::Working);
    record.since = {};
    CHECK(tabRowOf(rowsOf(oneTabWith(record))).time.kind == TimeKind::None);
}

TEST_CASE("SidebarTree.filter_keeps_ancestors_of_a_match", "[sidebar]")
{
    auto const directory = directoryOf({
        workspaceOf(1, "alpha", { tabOf(1, "build", { paneOf(1, "make"), paneOf(2, "tests") }) }),
        workspaceOf(2, "beta", { tabOf(2, "editor", { paneOf(3, "vim") }) }),
    });
    struct Case
    {
        std::string filter;
        std::vector<std::string> titles;
    };
    auto const cases = std::vector<Case> {
        { "TESTS", { "alpha", "build", "tests" } },
        { "EDIT", { "beta", "editor" } },
        { "beta", { "beta" } },
        { "zzz", {} },
        { "", { "alpha", "build", "make", "tests", "beta", "editor" } },
    };
    for (auto const& [filter, titles]: cases)
    {
        auto const rows = rowsOf(directory, SidebarState { .filter = filter });
        auto shown = std::vector<std::string> {};
        for (auto const& row: rows)
            shown.push_back(row.title);
        CHECK(shown == titles);
    }
    // The filter forces ancestors of a match open even when they are collapsed.
    auto const forced =
        rowsOf(directory,
               SidebarState {
                   .toggled = { workspaceKeyOf(1) }, .collapsedTabs = { tabKeyOf(1, 1) }, .filter = "make" });
    REQUIRE(forced.size() == 3);
    CHECK(forced[0].expansion == RowExpansion::Expanded);
    CHECK(forced[1].expansion == RowExpansion::Expanded);
}

TEST_CASE("SidebarTree.filter_matches_reason_and_git_text", "[sidebar]")
{
    auto workspace = workspaceOf(
        1,
        "w",
        { tabOf(1, "t", { paneOf(1, "p", { recordOf(ProgramState::Working, "Editing schema.rs") }) }) });
    workspace.git = DirectoryGit { .branch = "feat/Billing" };
    auto const directory = directoryOf({ workspace });
    CHECK(rowsOf(directory, SidebarState { .filter = "SCHEMA" }).size() == 2);
    CHECK(rowsOf(directory, SidebarState { .filter = "billing" }).size() == 1);
    CHECK(rowsOf(directory, SidebarState { .filter = "nothing" }).empty());
}

TEST_CASE("SidebarTree.footer_counts_leaves_not_roll_ups", "[sidebar]")
{
    auto const directory = directoryOf({
        workspaceOf(1,
                    "a",
                    { tabOf(1,
                            "t",
                            { paneOf(1, "p", { recordOf(ProgramState::Blocked) }),
                              paneOf(2, "q", { recordOf(ProgramState::Blocked) }),
                              paneOf(3, "r", { recordOf(ProgramState::Working) }) }),
                      tabOf(2, "u", { paneOf(4, "s", { recordOf(ProgramState::Working) }) }) }),
        workspaceOf(2, "b", { tabOf(3, "v", { paneOf(5, "z", { recordOf(ProgramState::Error) }) }) }),
    });
    auto const expected = SidebarFooter { .needYou = 2, .working = 2 };
    CHECK(sidebarFooter(rowsOf(directory)) == expected);
    // Folding hides rows, not panes.
    CHECK(
        sidebarFooter(rowsOf(directory, SidebarState { .toggled = { workspaceKeyOf(1), workspaceKeyOf(2) } }))
        == expected);
    CHECK(sidebarFooter({}) == SidebarFooter {});
}

TEST_CASE("SidebarTree.listing_counts_every_workspace_in_the_same_pass_as_a_filtered_tree", "[sidebar]")
{
    auto const directory = directoryOf({
        workspaceOf(1, "alpha", { tabOf(1, "t", { paneOf(1, "p", { recordOf(ProgramState::Blocked) }) }) }),
        workspaceOf(2, "beta", { tabOf(2, "u", { paneOf(2, "q", { recordOf(ProgramState::Working) }) }) }),
    });
    auto const listing =
        sidebarListing(directory, TabAttention {}, SidebarState { .filter = "beta" }, Now, std::nullopt);
    // The filter hides alpha's rows, not its workspace from the count.
    CHECK(listing.rows == rowsOf(directory, SidebarState { .filter = "beta" }));
    REQUIRE(listing.workspaces.size() == 2);
    CHECK(listing.workspaces[0].title == "alpha");
    CHECK(listing.workspaces[1].title == "beta");
    CHECK(sidebarFooter(listing.workspaces) == sidebarFooter(rowsOf(directory)));
    CHECK(sidebarSummary(listing.workspaces, workspaceKeyOf(2))
          == sidebarSummary(rowsOf(directory), workspaceKeyOf(2)));
}

TEST_CASE("SidebarTree.terminal_tree_lines", "[sidebar]")
{
    auto const rows = rowsOf(directoryOf({ workspaceOf(
        1, "w", { tabOf(1, "a", { paneOf(1), paneOf(2) }), tabOf(2, "b", { paneOf(3), paneOf(4) }) }) }));
    REQUIRE(rows.size() == 7);
    using enum TreeGlyph;
    auto const lines = [&](std::size_t index) {
        return rows[index].lines.columns;
    };
    CHECK(lines(0) == std::array { None, None });
    CHECK(lines(1) == std::array { Tee, None });  // tab a: later sibling
    CHECK(lines(2) == std::array { Bar, Tee });   // its first pane
    CHECK(lines(3) == std::array { Bar, Last });  // its last pane
    CHECK(lines(4) == std::array { Last, None }); // tab b: the last child
    CHECK(lines(5) == std::array { None, Tee });
    CHECK(lines(6) == std::array { None, Last });
}

TEST_CASE("SidebarTree.displayed_tab_is_selected", "[sidebar]")
{
    // Tab id 1 exists in both workspaces: a tab is named by its workspace and its id.
    auto const directory =
        directoryOf({ workspaceOf(1, "a", { tabOf(1, "t", { paneOf(1) }), tabOf(2, "u", { paneOf(2) }) }),
                      workspaceOf(2, "b", { tabOf(1, "v", { paneOf(3), paneOf(4) }) }) });
    auto const selections = [&](std::optional<TabRef> displayed) {
        auto result = std::vector<RowSelection> {};
        for (auto const& row: rowsOf(directory, {}, displayed))
            result.push_back(row.selection);
        return result;
    };
    using enum RowSelection;
    CHECK(selections(std::nullopt) == std::vector { None, None, None, None, None, None, None });
    CHECK(selections(TabRef { 1, 1 }) == std::vector { None, Selected, None, None, None, None, None });
    CHECK(selections(TabRef { 1, 2 }) == std::vector { None, None, Selected, None, None, None, None });
    CHECK(selections(TabRef { 2, 1 }) == std::vector { None, None, None, None, Selected, None, None });
    CHECK(selections(TabRef { 2, 2 }) == std::vector { None, None, None, None, None, None, None });

    // Folded away, the displayed tab hands its selection to its workspace, the nearest row shown.
    auto const folded = rowsOf(directory, SidebarState { .toggled = { workspaceKeyOf(2) } }, TabRef { 2, 1 });
    CHECK(std::ranges::to<std::vector>(folded | std::views::transform(&SidebarRow::selection))
          == std::vector { None, None, None, ContainsSelected });
}

TEST_CASE("SidebarTree.collapsing_a_tab_does_not_collapse_its_namesake", "[sidebar]")
{
    auto const directory = directoryOf({ workspaceOf(1, "a", { tabOf(1, "t", { paneOf(1), paneOf(2) }) }),
                                         workspaceOf(2, "b", { tabOf(1, "u", { paneOf(3), paneOf(4) }) }) });
    auto const rows = rowsOf(directory, SidebarState { .collapsedTabs = { tabKeyOf(2, 1) } });
    REQUIRE(rows.size() == 6);
    CHECK(rows[1].expansion == RowExpansion::Expanded);
    CHECK(rows[2].key.kind == SidebarRowKind::Pane);
    CHECK(rows[3].key.kind == SidebarRowKind::Pane);
    CHECK(rows[5].key.kind == SidebarRowKind::Tab);
    CHECK(rows[5].expansion == RowExpansion::Collapsed);
}

TEST_CASE("SidebarTree.rows_carry_their_real_positions", "[sidebar]")
{
    auto const directory = directoryOf({
        workspaceOf(
            5, "alpha", { tabOf(1, "one", { paneOf(1) }), tabOf(2, "two", { paneOf(2), paneOf(3) }) }),
        workspaceOf(6, "beta", { tabOf(3, "three", { paneOf(4) }), tabOf(4, "four", { paneOf(5) }) }),
    });
    auto const positions = [](std::vector<SidebarRow> const& rows) {
        auto result = std::vector<std::pair<std::string, std::size_t>> {};
        for (auto const& row: rows)
            result.emplace_back(row.title, row.position);
        return result;
    };
    using Entries = std::vector<std::pair<std::string, std::size_t>>;
    CHECK(positions(rowsOf(directory))
          == Entries { { "alpha", 0 },
                       { "one", 0 },
                       { "two", 1 },
                       { "Pane 1", 0 },
                       { "Pane 2", 1 },
                       { "beta", 1 },
                       { "three", 0 },
                       { "four", 1 } });
    // A filter hides rows, never their places.
    CHECK(positions(rowsOf(directory, SidebarState { .filter = "four" }))
          == Entries { { "beta", 1 }, { "four", 1 } });
    // Nor does a fold.
    CHECK(positions(rowsOf(directory, SidebarState { .toggled = { workspaceKeyOf(5) } }))
          == Entries { { "alpha", 0 }, { "beta", 1 }, { "three", 0 }, { "four", 1 } });
}

TEST_CASE("SidebarTree.reason_falls_back_to_the_title", "[sidebar]")
{
    auto record = recordOf(ProgramState::Working, "");
    record.title = "Editing schema.rs";
    CHECK(tabRowOf(rowsOf(oneTabWith(record))).reason == "Editing schema.rs");
    record.message = "Message wins";
    CHECK(tabRowOf(rowsOf(oneTabWith(record))).reason == "Message wins");
    record.message.clear();
    record.title.clear();
    CHECK(tabRowOf(rowsOf(oneTabWith(record))).reason.empty()); // never the app
}

TEST_CASE("SidebarTree.hostile_titles_and_branches_are_sanitized", "[sidebar]")
{
    auto const hostile = "a\x1b[31m" + bidiOverride() + "b" + zeroWidthSpace() + "c\nd";
    auto const rows = rowsOf(directoryOf({ [&] {
        auto workspace =
            workspaceOf(1, hostile, { tabOf(1, hostile, { paneOf(1, hostile), paneOf(2, hostile) }) });
        workspace.git = DirectoryGit { .branch = hostile + std::string(10'000, 'y'), .ahead = 1 };
        return workspace;
    }() }));
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].title == "a[31mbc d");
    CHECK(rows[1].title == "a[31mbc d");
    CHECK(rows[2].title == "a[31mbc d");
    CHECK(rows[0].gitText.starts_with("a[31mbc dy"));
    CHECK(rows[0].gitText.ends_with("… ↑1"));
    CHECK(rows[0].gitText.size() < 400);
}

TEST_CASE("SidebarTree.a_failed_command_has_a_reason_but_no_time", "[sidebar]")
{
    // vthost::proto::FinishedCommand carries no timestamp, so without the daemon's finish time nothing can be
    // measured from.
    auto pane = paneOf(1, "p");
    pane.finished = vthost::proto::FinishedCommand { .exitCode = 1, .durationMs = 18'000 };
    pane.finishedSerial = 1;
    auto const row = tabRowOf(rowsOf(directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { pane }) }) })));
    CHECK(row.reason == "exit 1 · 18s");
    CHECK(row.time == TimeText {});
}

TEST_CASE("SidebarTree.finished_command_shows_time_since_it_ended", "[sidebar][daemon-attention]")
{
    // A daemon keeps the pane's flags and stated when the command ended: the row says how long ago.
    auto pane = paneOf(1, "p");
    pane.flagSource = FlagSource::Daemon;
    pane.attention = AttentionKnowledge::Stated;
    pane.finished = vthost::proto::FinishedCommand { .exitCode = 1, .durationMs = 18'000 };
    pane.finishedSerial = 1;
    pane.finishedAt = Now - std::chrono::minutes { 3 };
    auto const row = tabRowOf(rowsOf(directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { pane }) }) })));
    CHECK(row.reason == "exit 1 · 18s");
    CHECK(row.time.kind == TimeKind::Relative);
    CHECK(row.time.text == formatRelative(std::chrono::minutes { 3 }));
}

TEST_CASE("SidebarTree.git_text", "[sidebar]")
{
    struct Case
    {
        std::optional<uint32_t> ahead;
        std::optional<uint32_t> behind;
        std::string text;
    };
    auto const cases = std::vector<Case> {
        { 2, 1, "feat/billing ↑2 ↓1" },
        { 2, 0, "feat/billing ↑2" },
        { 0, 3, "feat/billing ↓3" },
        { 0, 0, "feat/billing" },
        { std::nullopt, std::nullopt, "feat/billing" },
    };
    for (auto const& [ahead, behind, text]: cases)
    {
        auto workspace = workspaceOf(1, "w", {});
        workspace.git = DirectoryGit { .branch = "feat/billing", .ahead = ahead, .behind = behind };
        CHECK(rowsOf(directoryOf({ workspace })).at(0).gitText == text);
    }
    CHECK(rowsOf(directoryOf({ workspaceOf(1, "w", {}) })).at(0).gitText.empty());
}

TEST_CASE("SidebarTree.a_seen_ledger_quiets_a_row", "[sidebar]")
{
    auto const directory = oneTabWith(recordOf(ProgramState::Done, "ok"));
    auto attention = TabAttention {};
    CHECK(sidebarRows(directory, attention, {}, Now, std::nullopt).at(1).urgency == Urgency::DoneUnseen);
    auto const changed = attention.noteSeen(PaneKey { 1 }, 1);
    CHECK(changed == AttentionChange::Changed);
    CHECK(sidebarRows(directory, attention, {}, Now, std::nullopt).at(1).urgency == Urgency::Idle);
}

TEST_CASE("SidebarTree.a_permission_reads_as_a_command_with_its_program_and_folder", "[sidebar]")
{
    auto directory = oneTabWith(recordOf(ProgramState::Blocked, "Bash: psql", 42s, BlockedKind::Permission));
    directory.workspaces[0].tabs[0].panes[0].cwd = "/src/billing";
    auto const rows = rowsOf(directory);
    auto const& tab = tabRowOf(rows);
    CHECK(tab.reason == "Permission · Bash: psql");
    CHECK(tab.reasonLabel == "Permission · Bash: ");
    CHECK(tab.reasonCommand == "psql");
    CHECK(tab.fullReasonLabel == "Permission · Bash: ");
    CHECK(tab.fullReasonCommand == "psql");
    CHECK(tab.program == "app");
    CHECK(tab.folder == "/src/billing");

    // A question is words, not a command; a workspace row names no program of its own.
    auto const asked =
        rowsOf(oneTabWith(recordOf(ProgramState::Blocked, "Proceed?", 42s, BlockedKind::Question)));
    CHECK(tabRowOf(asked).reasonLabel == "Question · Proceed?");
    CHECK(tabRowOf(asked).reasonCommand.empty());
    CHECK(rows.front().program.empty());
}

TEST_CASE("SidebarTree.the_summary_names_the_shown_workspace_and_rolls_up_the_others", "[sidebar]")
{
    auto const directory = directoryOf({
        workspaceOf(
            1, "billing", { tabOf(10, "a", { paneOf(10, "a", { recordOf(ProgramState::Blocked) }) }) }),
        workspaceOf(2, "docs", { tabOf(20, "b", { paneOf(20, "b", { recordOf(ProgramState::Working) }) }) }),
    });
    auto const rows = rowsOf(directory);

    auto const onDocs = sidebarSummary(rows, workspaceKeyOf(2));
    CHECK(onDocs.currentWorkspace == "docs");
    CHECK(onDocs.otherWorkspaces == Urgency::NeedsYou);
    CHECK(onDocs.otherWorkspacesGlyph == rows.front().glyph);
    CHECK(chipMarkerOf(onDocs.otherWorkspaces) == ChipMarker::Shown);

    // Work running elsewhere does not mark the chip.
    auto const onBilling = sidebarSummary(rows, workspaceKeyOf(1));
    CHECK(onBilling.currentWorkspace == "billing");
    CHECK(onBilling.otherWorkspaces == Urgency::Working);
    CHECK(chipMarkerOf(onBilling.otherWorkspaces) == ChipMarker::None);

    // No shown workspace: no name, and every workspace counts.
    CHECK(sidebarSummary(rows, std::nullopt).currentWorkspace.empty());
    CHECK(sidebarSummary(rows, std::nullopt).otherWorkspaces == Urgency::NeedsYou);
}

TEST_CASE("SidebarTree.chip_marker_from_done_unseen_up", "[sidebar]")
{
    CHECK(chipMarkerOf(Urgency::Idle) == ChipMarker::None);
    CHECK(chipMarkerOf(Urgency::UnseenOutput) == ChipMarker::None);
    CHECK(chipMarkerOf(Urgency::Working) == ChipMarker::None);
    CHECK(chipMarkerOf(Urgency::DoneUnseen) == ChipMarker::Shown);
    CHECK(chipMarkerOf(Urgency::Failed) == ChipMarker::Shown);
    CHECK(chipMarkerOf(Urgency::NeedsYou) == ChipMarker::Shown);
}

TEST_CASE("SidebarTree.parent_row_of", "[sidebar]")
{
    auto const rows = rowsOf(directoryOf({ workspaceOf(
        1, "w", { tabOf(1, "split", { paneOf(1, "a"), paneOf(2, "b") }), tabOf(2, "t", { paneOf(3) }) }) }));
    REQUIRE(rows.size() == 5); // w, split, a, b, t
    CHECK(parentRowOf(rows, 0) == -1);
    CHECK(parentRowOf(rows, 1) == 0);
    CHECK(parentRowOf(rows, 2) == 1);
    CHECK(parentRowOf(rows, 3) == 1);
    CHECK(parentRowOf(rows, 4) == 0);
    CHECK(parentRowOf(rows, 5) == -1);
    CHECK(parentRowOf(rows, -1) == -1);
}

TEST_CASE("SidebarTree.tree_lines_text_is_two_cells_a_column", "[sidebar]")
{
    auto const rows = rowsOf(directoryOf({ workspaceOf(
        1, "w", { tabOf(1, "split", { paneOf(1, "a"), paneOf(2, "b") }), tabOf(2, "t", { paneOf(3) }) }) }));
    REQUIRE(rows.size() == 5);
    CHECK(treeLinesText(rows[0].lines, rows[0].depth).empty());
    CHECK(treeLinesText(rows[1].lines, rows[1].depth) == "├ ");
    CHECK(treeLinesText(rows[2].lines, rows[2].depth) == "│ ├ ");
    CHECK(treeLinesText(rows[3].lines, rows[3].depth) == "│ └ ");
    CHECK(treeLinesText(rows[4].lines, rows[4].depth) == "└ ");
}

TEST_CASE("SidebarTree.tree_lines_connect_through_second_lines", "[sidebar]")
{
    // Every row drawn as two lines -- its first, and the continuation its reason line carries -- the way a
    // tree(1) drawing reads: a line leaving a cell downwards arrives in the cell below, and a line arriving
    // from above left the cell above, or hangs from the row whose children it leads to.
    struct Line
    {
        TreeLines glyphs;
        uint8_t depth = 0;
        bool first = false;
    };
    auto const sends = [](TreeGlyph glyph) {
        return glyph == TreeGlyph::Tee || glyph == TreeGlyph::Bar;
    };
    auto const receives = [](TreeGlyph glyph) {
        return glyph != TreeGlyph::None;
    };

    auto const rows =
        rowsOf(directoryOf({ workspaceOf(1,
                                         "w",
                                         { tabOf(1, "split", { paneOf(1, "a"), paneOf(2, "b") }),
                                           tabOf(2, "t", { paneOf(3) }),
                                           tabOf(3, "last", { paneOf(4, "c"), paneOf(5, "d") }) }),
                             workspaceOf(2, "v", { tabOf(4, "only", { paneOf(6) }) }) }));
    auto lines = std::vector<Line> {};
    for (auto const& row: rows)
    {
        lines.push_back({ .glyphs = row.lines, .depth = row.depth, .first = true });
        if (row.depth > 0)
            lines.push_back({ .glyphs = row.continued, .depth = row.depth, .first = false });
    }
    for (auto const index: std::views::iota(std::size_t { 1 }, lines.size()))
    {
        auto const& above = lines[index - 1];
        auto const& below = lines[index];
        for (auto const column: std::views::iota(std::size_t { 0 }, below.glyphs.columns.size()))
        {
            INFO("line " << index << ", column " << column);
            if (sends(above.glyphs.columns[column]))
                CHECK(receives(below.glyphs.columns[column]));
            if (receives(below.glyphs.columns[column]))
                CHECK((sends(above.glyphs.columns[column]) || (above.first && above.depth == column)));
        }
    }
    // The line from a tab down to its panes runs through the tab's second line.
    CHECK(treeContinuationText(rows[1]) == "│ │ ");
    CHECK(treeContinuationText(rows[3]) == "│   ");
    CHECK(treeContinuationText(rows[0]).empty());
}

TEST_CASE("SidebarTree.workspace_rail_cells_run_unbroken", "[sidebar]")
{
    // A workspace's rail on the Terminal look is the tree's first cell on every line of its rows: drawn as
    // the tree's own connector, a blank (a pane under the last tab, the last tab's second line) and a corner
    // (whose stroke stops half way down) made to run on, so no line of the workspace is left without it.
    auto const rows =
        rowsOf(directoryOf({ workspaceOf(1,
                                         "w",
                                         { tabOf(1, "split", { paneOf(1, "a"), paneOf(2, "b") }),
                                           tabOf(2, "last", { paneOf(3, "c"), paneOf(4, "d") }) }),
                             workspaceOf(2, "empty", {}) }));
    REQUIRE(rows.size() == 8);
    auto const cells = std::ranges::to<std::vector>(rows | std::views::transform(workspaceRailCellsOf));

    // The workspace row: its disclosure is the cell, and draws itself.
    CHECK(cells[0] == RailCells { .first = "", .second = "" });
    CHECK(cells[1] == RailCells { .first = "├", .second = "│" });
    CHECK(cells[2] == RailCells { .first = "│", .second = "│" });
    CHECK(cells[3] == RailCells { .first = "│", .second = "│" });
    // The last tab: its corner runs on, and so do its blank second line and the cells of its panes.
    CHECK(treeLinesText(rows[4].lines, rows[4].depth).starts_with("└"));
    CHECK(treeContinuationText(rows[4]).starts_with(" "));
    CHECK(cells[4] == RailCells { .first = "├", .second = "│" });
    CHECK(treeLinesText(rows[5].lines, rows[5].depth).starts_with(" "));
    CHECK(cells[5] == RailCells { .first = "│", .second = "│" });
    CHECK(cells[6] == RailCells { .first = "│", .second = "│" });
    // A workspace with nothing to fold has no disclosure: the rail draws its cell, on its one line.
    REQUIRE(rows[7].expansion == RowExpansion::None);
    CHECK(cells[7] == RailCells { .first = "│", .second = "" });

    // Nothing the tree itself draws changes: the rail is drawn over it, not into it.
    CHECK(treeLinesText(rows[4].lines, rows[4].depth) == "└ ");
    CHECK(treeContinuationText(rows[5]) == "  │ ");
}

TEST_CASE("SidebarTree.meter_text_fills_eighths_of_cells", "[sidebar]")
{
    CHECK(meterText(0) == "░░░░░");
    CHECK(meterText(100) == "█████");
    CHECK(meterText(50) == "██▌░░");
    CHECK(meterText(-5) == meterText(0));
    CHECK(meterText(250) == meterText(100));
}

TEST_CASE("SidebarTree.urgency_has_no_words_when_idle", "[sidebar]")
{
    CHECK(urgencyText(Urgency::Idle).empty());
}

TEST_CASE("SidebarTree.hover_detail_joins_what_it_has", "[sidebar]")
{
    auto directory = oneTabWith(recordOf(ProgramState::Working));
    directory.workspaces[0].tabs[0].panes[0].cwd = "/src";
    auto const rows = rowsOf(directory);
    CHECK(hoverDetailOf(tabRowOf(rows)) == "app · /src · 0:42");
    CHECK(hoverDetailOf(rows.front()).empty());
}

TEST_CASE("the hover detail holds a long folder whole", "[sidebar]")
{
    auto const cwd = "/" + std::string(299, 'd');
    auto directory = oneTabWith(recordOf(ProgramState::Working));
    directory.workspaces[0].tabs[0].panes[0].cwd = cwd;
    auto const rows = rowsOf(directory);
    auto const& row = tabRowOf(rows);
    // The row's own line stays cut at MaxRowTextLength (the ellipsis counts as one character).
    CHECK(row.folder == cwd.substr(0, MaxRowTextLength - 1) + "…");
    CHECK(hoverDetailOf(row).contains(cwd));
}

namespace
{
/// A row of @p depth with only the parts the second line is decided from.
SidebarRow secondLineRow(uint8_t depth, std::string folder, std::string program, std::string reason = {})
{
    auto row = SidebarRow {};
    row.depth = depth;
    row.folder = std::move(folder);
    row.program = std::move(program);
    row.reason = std::move(reason);
    return row;
}
} // namespace

TEST_CASE("idleLineOf joins folder and program", "[sidebar]")
{
    CHECK(idleLineOf(secondLineRow(1, "~/src", "claude")) == "~/src · claude");
    CHECK(idleLineOf(secondLineRow(2, "~/src", "claude")) == "~/src · claude");
}

TEST_CASE("idleLineOf leaves a missing half out", "[sidebar]")
{
    CHECK(idleLineOf(secondLineRow(1, "~/src", {})) == "~/src");
    CHECK(idleLineOf(secondLineRow(1, {}, "claude")) == "claude");
    CHECK(idleLineOf(secondLineRow(1, {}, {})).empty());
}

TEST_CASE("idleLineOf is empty for a workspace row", "[sidebar]")
{
    CHECK(idleLineOf(secondLineRow(0, "~/src", "claude")).empty());
}

TEST_CASE("secondLineOf prefers a status to the idle line", "[sidebar]")
{
    for (auto const depth: { uint8_t { 1 }, uint8_t { 2 } })
    {
        CHECK(secondLineOf(secondLineRow(depth, "~/src", {}, "exit 1")) == SecondLine::Status);
        CHECK(secondLineOf(secondLineRow(depth, "~/src", {})) == SecondLine::Idle);
        CHECK(secondLineOf(secondLineRow(depth, {}, "claude")) == SecondLine::Idle);
        CHECK(secondLineOf(secondLineRow(depth, {}, {}, "exit 1")) == SecondLine::Status);
    }
    CHECK(secondLineOf(secondLineRow(0, {}, {}, "exit 1")) == SecondLine::None);
    CHECK(secondLineOf(secondLineRow(0, {}, {})) == SecondLine::None);
}

TEST_CASE("secondLineOf has nothing for a row that knows neither its folder nor its program", "[sidebar]")
{
    // The row keeps its two-line height and centres its name in it (SidebarRow.qml).
    for (auto const depth: { uint8_t { 1 }, uint8_t { 2 } })
    {
        CHECK(secondLineOf(secondLineRow(depth, {}, {})) == SecondLine::None);
        CHECK(idleLineOf(secondLineRow(depth, {}, {})).empty());
    }
}

TEST_CASE("SidebarTree.unlabelled_workspace_is_numbered_as_its_host_numbers_it", "[sidebar]")
{
    // The daemon counts a workspace the list leaves out: the row says what `contour ls` says.
    auto workspace = workspaceOf(9, {}, { tabOf(1, "t", { paneOf(1) }) });
    workspace.position = 2;
    auto const rows =
        rowsOf(directoryOf({ workspaceOf(8, "ops", { tabOf(2, "u", { paneOf(2) }) }), workspace }));
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].title == "ops");
    CHECK(rows[2].title == "Workspace 3");
}

TEST_CASE("a coloured tab's row and its pane rows carry its colour", "[sidebar]")
{
    auto constexpr Orange = vtbackend::RGBColor { 0xE8, 0x85, 0x3A };
    auto coloured = tabOf(1, "t", { paneOf(1, "a"), paneOf(2, "b") });
    coloured.color = Orange;
    auto const rows =
        rowsOf(directoryOf({ workspaceOf(1, "w", { std::move(coloured), tabOf(2, "u", { paneOf(3) }) }) }));
    REQUIRE(rows.size() == 5);
    CHECK(rows[1].key.kind == SidebarRowKind::Tab);
    CHECK(rows[1].tabColor == Orange);
    CHECK(rows[2].key.kind == SidebarRowKind::Pane);
    CHECK(rows[2].tabColor == Orange);
    CHECK(rows[3].key.kind == SidebarRowKind::Pane);
    CHECK(rows[3].tabColor == Orange);
    // A tab without a colour leaves its row without one.
    CHECK(rows[4].key.kind == SidebarRowKind::Tab);
    CHECK_FALSE(rows[4].tabColor.has_value());
}

TEST_CASE("a workspace row carries no tab colour", "[sidebar]")
{
    auto coloured = tabOf(1, "t", { paneOf(1) });
    coloured.color = vtbackend::RGBColor { 0xE8, 0x85, 0x3A };
    auto const directory = directoryOf({ workspaceOf(1, "w", { std::move(coloured) }) });

    auto const expanded = rowsOf(directory);
    REQUIRE(expanded.size() == 2);
    CHECK_FALSE(expanded[0].tabColor.has_value());

    // Folded over its coloured tab, the workspace row still shows none of it.
    auto const folded = rowsOf(directory, SidebarState { .toggled = { workspaceKeyOf(1) } });
    REQUIRE(folded.size() == 1);
    CHECK(folded[0].key.kind == SidebarRowKind::Workspace);
    CHECK_FALSE(folded[0].tabColor.has_value());
}

TEST_CASE("every row of a coloured workspace carries its colour", "[sidebar]")
{
    auto constexpr Magenta = vtbackend::RGBColor { 0xC2, 0x54, 0x8F };
    auto coloured = workspaceOf(
        1, "w", { tabOf(1, "t", { paneOf(1, "a"), paneOf(2, "b") }), tabOf(2, "u", { paneOf(3) }) });
    coloured.color = Magenta;
    auto const rows = rowsOf(directoryOf({ std::move(coloured) }));
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].key.kind == SidebarRowKind::Workspace);
    CHECK(rows[1].key.kind == SidebarRowKind::Tab);
    CHECK(rows[2].key.kind == SidebarRowKind::Pane);
    CHECK(rows[3].key.kind == SidebarRowKind::Pane);
    CHECK(rows[4].key.kind == SidebarRowKind::Tab);
    for (auto const& row: rows)
        CHECK(row.workspaceColor == Magenta);
}

TEST_CASE("a folded coloured workspace keeps its colour", "[sidebar]")
{
    auto coloured = workspaceOf(1, "w", { tabOf(1, "t", { paneOf(1) }) });
    coloured.color = vtbackend::RGBColor { 0xC2, 0x54, 0x8F };
    auto const folded =
        rowsOf(directoryOf({ std::move(coloured) }), SidebarState { .toggled = { workspaceKeyOf(1) } });
    REQUIRE(folded.size() == 1);
    CHECK(folded[0].key.kind == SidebarRowKind::Workspace);
    CHECK(folded[0].workspaceColor == vtbackend::RGBColor { 0xC2, 0x54, 0x8F });
}

TEST_CASE("an uncoloured workspace's rows carry none", "[sidebar]")
{
    auto coloured = workspaceOf(1, "w", { tabOf(1, "t", { paneOf(1) }) });
    coloured.color = vtbackend::RGBColor { 0xC2, 0x54, 0x8F };
    auto const rows = rowsOf(
        directoryOf({ std::move(coloured),
                      workspaceOf(2, "plain", { tabOf(2, "u", { paneOf(2, "a"), paneOf(3, "b") }) }) }));
    REQUIRE(rows.size() == 6);
    CHECK(rows[2].key.workspace == 2);
    for (auto const& row: rows | std::views::drop(2))
        CHECK_FALSE(row.workspaceColor.has_value());
}

// ---------------------------------------------------------------------------------------------------------------
// Several daemons, each under its section heading.

namespace
{
DaemonRef daemonA()
{
    return localDaemonRef("a");
}

DaemonRef daemonB()
{
    return hostDaemonRef("b");
}

DaemonSection sectionOf(DaemonRef daemon,
                        uint64_t instance,
                        WorkspaceDirectory directory,
                        DaemonLinkState state = DaemonLinkState::Connected)
{
    auto section = DaemonSection {};
    section.name = std::string { parseDaemonRef(daemon)->second };
    section.daemon = std::move(daemon);
    section.instance = DaemonInstance { instance };
    section.directory = std::move(directory);
    section.state = state;
    return section;
}

/// Two workspaces of one tab of one pane each, with the ids every daemon reuses.
WorkspaceDirectory pairOf(ProgramState first = ProgramState::Idle, ProgramState second = ProgramState::Idle)
{
    return directoryOf(
        { workspaceOf(1, "one", { tabOf(1, "t1", { paneOf(1, "p1", { recordOf(first) }) }) }),
          workspaceOf(2, "two", { tabOf(1, "t2", { paneOf(2, "p2", { recordOf(second) }) }) }) });
}

SidebarTarget targetOf(DaemonRef daemon, uint64_t workspace, std::optional<uint64_t> tab)
{
    return SidebarTarget { .daemon = std::move(daemon),
                           .instance = DaemonInstance { 1 },
                           .workspace = workspace,
                           .tab = tab,
                           .pane = {} };
}

WorkspaceKey workspaceKeyOn(DaemonRef daemon, uint64_t instance, uint64_t workspace)
{
    return WorkspaceKey { .daemon = std::move(daemon),
                          .instance = DaemonInstance { instance },
                          .workspace = workspace };
}

SidebarState openedOn(DaemonRef daemon)
{
    auto state = SidebarState {};
    state.openedOn = std::move(daemon);
    return state;
}

SidebarListing listingOf(std::vector<DaemonSection> const& sections,
                         SidebarState const& state,
                         std::optional<SidebarTarget> displayed = std::nullopt,
                         std::optional<SidebarTarget> pending = std::nullopt,
                         std::vector<HiddenDaemon> const& hidden = {})
{
    return sidebarListing(
        SidebarSources { .sections = sections, .own = daemonA(), .ownMarks = nullptr, .hidden = hidden },
        state,
        Now,
        std::move(displayed),
        std::move(pending));
}

/// A row as one line: its kind's letter, then its daemon, workspace, tab and pane where it has them.
std::string describe(SidebarRow const& row)
{
    auto const& key = row.key;
    switch (key.kind)
    {
        case SidebarRowKind::Daemon: return "D " + key.daemon.value;
        case SidebarRowKind::Workspace: return std::format("W {} {}", key.daemon.value, key.workspace);
        case SidebarRowKind::Tab: return std::format("T {} {} {}", key.daemon.value, key.workspace, key.tab);
        case SidebarRowKind::Pane:
            return std::format("P {} {} {} {}", key.daemon.value, key.workspace, key.tab, key.pane.value);
        case SidebarRowKind::Hidden: return "H";
    }
    return {};
}

std::vector<std::string> describe(std::vector<SidebarRow> const& rows)
{
    return std::ranges::to<std::vector>(
        rows | std::views::transform([](SidebarRow const& row) { return describe(row); }));
}

/// @return The index of the row described as @p description; -1 when none is.
int indexOf(std::vector<SidebarRow> const& rows, std::string const& description)
{
    auto const all = describe(rows);
    auto const found = std::ranges::find(all, description);
    return found == all.end() ? -1 : static_cast<int>(std::distance(all.begin(), found));
}

SidebarRow const& rowAt(std::vector<SidebarRow> const& rows, std::string const& description)
{
    auto const index = indexOf(rows, description);
    REQUIRE(index >= 0);
    return rows[static_cast<std::size_t>(index)];
}
} // namespace

TEST_CASE("SidebarTree.daemon_rows_head_each_section", "[sidebar]")
{
    auto const solo = directoryOf({ workspaceOf(1, "solo", { tabOf(1, "t", { paneOf(1) }) }) });
    auto const rows =
        listingOf({ sectionOf(daemonA(), 1, pairOf()), sectionOf(daemonB(), 1, solo) }, openedOn(daemonA()))
            .rows;
    CHECK(describe(rows)
          == std::vector<std::string> { "D local:a",
                                        "W local:a 1",
                                        "T local:a 1 1",
                                        "W local:a 2",
                                        "T local:a 2 1",
                                        "D host:b",
                                        "W host:b 1" });
    for (auto const& row: rows)
    {
        if (row.key.kind == SidebarRowKind::Tab)
            continue;
        CHECK(row.depth == 0);
    }
    CHECK(rows[0].title == "a");
    CHECK(rows[5].title == "b");
    CHECK(rows[2].daemonName == "a");
    CHECK(rows[6].daemonName == "b");
}

TEST_CASE("SidebarTree.order_is_fixed_and_independent_of_the_window", "[sidebar]")
{
    auto const sections =
        std::vector { sectionOf(daemonA(), 1, pairOf()), sectionOf(daemonB(), 1, pairOf()) };
    // The window is on b and shows one of its tabs: b stays second.
    auto const listing = sidebarListing(
        SidebarSources { .sections = sections, .own = daemonB(), .ownMarks = nullptr, .hidden = {} },
        openedOn(daemonB()),
        Now,
        targetOf(daemonB(), 1, 1),
        std::nullopt);
    auto const daemons = std::ranges::to<std::vector>(
        listing.rows
        | std::views::filter([](SidebarRow const& row) { return row.key.kind == SidebarRowKind::Daemon; })
        | std::views::transform(&SidebarRow::title));
    CHECK(daemons == std::vector<std::string> { "a", "b" });
    CHECK(rowAt(listing.rows, "T host:b 1 1").selection == RowSelection::Selected);
}

TEST_CASE("SidebarTree.other_daemons_start_folded_and_a_jump_refolds_nothing", "[sidebar]")
{
    auto const sections =
        std::vector { sectionOf(daemonA(), 1, pairOf()), sectionOf(daemonB(), 1, pairOf()) };
    auto const state = openedOn(daemonA());
    auto const rows = listingOf(sections, state).rows;
    CHECK(rowAt(rows, "W local:a 1").expansion == RowExpansion::Expanded);
    CHECK(rowAt(rows, "W local:a 2").expansion == RowExpansion::Expanded);
    CHECK(rowAt(rows, "W host:b 1").expansion == RowExpansion::Collapsed);
    CHECK(rowAt(rows, "W host:b 2").expansion == RowExpansion::Collapsed);

    // Displaying a tab of b's unfolds nothing: its folded workspace carries the selection.
    auto const jumped = listingOf(sections, state, targetOf(daemonB(), 1, 1)).rows;
    CHECK(rowAt(jumped, "W host:b 1").expansion == RowExpansion::Collapsed);
    CHECK(rowAt(jumped, "W host:b 1").selection == RowSelection::ContainsSelected);
    CHECK(rowAt(jumped, "D host:b").selection == RowSelection::None);
    CHECK(indexOf(jumped, "T host:b 1 1") == -1);

    // A folded daemon row holding the displayed tab carries it instead.
    auto foldedB = state;
    foldedB.foldedDaemons = { daemonB() };
    auto const folded = listingOf(sections, foldedB, targetOf(daemonB(), 1, 1)).rows;
    CHECK(rowAt(folded, "D host:b").expansion == RowExpansion::Collapsed);
    CHECK(rowAt(folded, "D host:b").selection == RowSelection::ContainsSelected);
    CHECK(indexOf(folded, "W host:b 1") == -1);
}

TEST_CASE("SidebarTree.a_workspace_appearing_later_follows_its_daemon_default", "[sidebar]")
{
    auto state = openedOn(daemonA());
    state.toggled = { workspaceKeyOn(daemonB(), 1, 1) };
    auto grown = pairOf();
    grown.workspaces.push_back(workspaceOf(3, "three", { tabOf(1, "t3", { paneOf(3) }) }));
    auto const rows =
        listingOf({ sectionOf(daemonA(), 1, grown), sectionOf(daemonB(), 1, grown) }, state).rows;
    CHECK(rowAt(rows, "W host:b 1").expansion == RowExpansion::Expanded);
    CHECK(rowAt(rows, "W host:b 3").expansion == RowExpansion::Collapsed);
    CHECK(rowAt(rows, "W local:a 3").expansion == RowExpansion::Expanded);
}

TEST_CASE("SidebarTree.same_ids_on_two_daemons_have_distinct_keys", "[sidebar]")
{
    auto state = openedOn(localDaemonRef("elsewhere"));
    state.toggled = { workspaceKeyOn(daemonA(), 1, 1) };
    auto const rows =
        listingOf({ sectionOf(daemonA(), 1, pairOf()), sectionOf(daemonB(), 1, pairOf()) }, state).rows;
    auto const& onA = rowAt(rows, "W local:a 1");
    auto const& onB = rowAt(rows, "W host:b 1");
    CHECK(onA.key != onB.key);
    CHECK(onA.expansion == RowExpansion::Expanded);
    CHECK(onB.expansion == RowExpansion::Collapsed);
}

TEST_CASE("SidebarTree.a_restarted_daemon_does_not_inherit_toggles", "[sidebar]")
{
    auto state = openedOn(daemonA());
    state.toggled = { workspaceKeyOn(daemonA(), 1, 1) };
    CHECK(rowAt(listingOf({ sectionOf(daemonA(), 1, pairOf()) }, state).rows, "W local:a 1").expansion
          == RowExpansion::Collapsed);
    CHECK(rowAt(listingOf({ sectionOf(daemonA(), 2, pairOf()) }, state).rows, "W local:a 1").expansion
          == RowExpansion::Expanded);
}

TEST_CASE("SidebarTree.status_slot_shows_link_state_until_connected", "[sidebar]")
{
    for (auto const index: std::views::iota(size_t { 0 }, DaemonLinkStateCount))
    {
        auto const state = DaemonLinkState(index);
        auto const rows =
            listingOf({ sectionOf(daemonA(), 1, pairOf(ProgramState::Blocked), state) }, openedOn(daemonA()))
                .rows;
        auto const expected = state == DaemonLinkState::Connected ? StatusSlot::Urgency : StatusSlot::Link;
        CHECK(rowAt(rows, "D local:a").slot == expected);
        CHECK(rowAt(rows, "D local:a").linkState == state);
        CHECK(statusSlotOf(state, RowPending::Yes, Urgency::Idle) == StatusSlot::Pending);
    }
    auto const idle = listingOf({ sectionOf(daemonA(), 1, pairOf()) }, openedOn(daemonA())).rows;
    CHECK(rowAt(idle, "D local:a").slot == StatusSlot::None);
    CHECK(statusSlotOf(DaemonLinkState::Connected, RowPending::No, Urgency::Working) == StatusSlot::Urgency);
}

TEST_CASE("SidebarTree.daemon_row_rolls_up_most_urgent_glyph", "[sidebar]")
{
    auto const sections =
        std::vector { sectionOf(daemonA(), 1, pairOf()),
                      sectionOf(daemonB(), 1, pairOf(ProgramState::Working, ProgramState::Blocked)) };
    auto foldedB = openedOn(daemonA());
    foldedB.foldedDaemons = { daemonB() };
    for (auto const& state: { openedOn(daemonA()), openedOn(daemonB()), foldedB })
    {
        auto const& row = rowAt(listingOf(sections, state).rows, "D host:b");
        CHECK(row.urgency == Urgency::NeedsYou);
        CHECK(row.glyph == TabBadgeKind::Blocked);
        CHECK(rowAt(listingOf(sections, state).rows, "D local:a").urgency == Urgency::Idle);
    }
}

TEST_CASE("SidebarTree.needs_me_keeps_displayed_and_pending_paths", "[sidebar]")
{
    auto onA = pairOf(ProgramState::Blocked);
    onA.workspaces[0].tabs.push_back(tabOf(2,
                                           "split",
                                           { paneOf(11, "bad", { recordOf(ProgramState::Error) }),
                                             paneOf(12, "fine", { recordOf(ProgramState::Idle) }) }));
    auto state = openedOn(daemonA());
    state.scope = contour::config::SidebarScope::NeedsMe;
    auto const rows = listingOf({ sectionOf(daemonA(), 1, onA), sectionOf(daemonB(), 1, pairOf()) },
                                state,
                                targetOf(daemonA(), 2, 1),
                                targetOf(daemonB(), 1, 1))
                          .rows;
    CHECK(describe(rows)
          == std::vector<std::string> { "D local:a",
                                        "W local:a 1",
                                        "T local:a 1 1",
                                        "T local:a 1 2",
                                        "P local:a 1 2 11",
                                        "W local:a 2",
                                        "T local:a 2 1",
                                        "D host:b",
                                        "W host:b 1",
                                        "T host:b 1 1" });
    // Ancestors of what stays are expanded, b's default fold notwithstanding.
    CHECK(rowAt(rows, "W host:b 1").expansion == RowExpansion::Expanded);
    CHECK(rowAt(rows, "D host:b").expansion == RowExpansion::Expanded);
    CHECK(needsMe(Urgency::NeedsYou));
    CHECK(needsMe(Urgency::Failed));
    CHECK(needsMe(Urgency::DoneUnseen));
    CHECK_FALSE(needsMe(Urgency::Working));
    CHECK_FALSE(needsMe(Urgency::UnseenOutput));
    CHECK_FALSE(needsMe(Urgency::Idle));
}

TEST_CASE("SidebarTree.needs_me_empty", "[sidebar]")
{
    auto state = openedOn(daemonA());
    state.scope = contour::config::SidebarScope::NeedsMe;
    auto const listing = listingOf(
        { sectionOf(daemonA(), 1, pairOf(ProgramState::Working)), sectionOf(daemonB(), 1, pairOf()) },
        state,
        std::nullopt,
        std::nullopt,
        { HiddenDaemon { .daemon = hostDaemonRef("c"), .name = "c" } });
    CHECK(listing.rows.empty());
    CHECK(listing.workspaces.size() == 4);
    CHECK(sidebarFooter(listing.workspaces) == SidebarFooter { .needYou = 0, .working = 1 });
}

TEST_CASE("SidebarTree.empty_state_only_under_needs_me", "[sidebar]")
{
    using contour::config::SidebarScope;
    CHECK(sidebarEmptyStateOf(SidebarScope::NeedsMe, 0) == SidebarEmptyState::NothingNeedsYou);
    CHECK(sidebarEmptyStateOf(SidebarScope::NeedsMe, 1) == SidebarEmptyState::None);
    // Everything listed has nothing to say about what needs the user, empty or not.
    CHECK(sidebarEmptyStateOf(SidebarScope::All, 0) == SidebarEmptyState::None);
    CHECK(sidebarEmptyStateOf(SidebarScope::All, 3) == SidebarEmptyState::None);
    CHECK(toggledScope(SidebarScope::All) == SidebarScope::NeedsMe);
    CHECK(toggledScope(SidebarScope::NeedsMe) == SidebarScope::All);

    // The displayed tab's path stays under Needs-me, so rows remain while nothing needs the user: the rows
    // that need the user decide, not the rows listed.
    auto state = openedOn(daemonA());
    state.scope = SidebarScope::NeedsMe;
    auto const quiet = listingOf({ sectionOf(daemonA(), 1, pairOf(ProgramState::Working)) },
                                 state,
                                 targetOf(daemonA(), 1, 1))
                           .rows;
    CHECK_FALSE(quiet.empty());
    CHECK(rowsNeedingMe(quiet) == 0);
    auto const asking = listingOf({ sectionOf(daemonA(), 1, pairOf(ProgramState::Blocked)) },
                                  state,
                                  targetOf(daemonA(), 2, 1))
                            .rows;
    CHECK(rowsNeedingMe(asking) > 0);
}

TEST_CASE("SidebarTree.breadcrumb_names_the_daemon_only_beside_another", "[sidebar]")
{
    CHECK(breadcrumbDepthOf(0) == BreadcrumbDepth::WorkspaceTab);
    CHECK(breadcrumbDepthOf(1) == BreadcrumbDepth::WorkspaceTab);
    CHECK(breadcrumbDepthOf(2) == BreadcrumbDepth::DaemonWorkspaceTab);
    CHECK(breadcrumbDepthOf(7) == BreadcrumbDepth::DaemonWorkspaceTab);
}

TEST_CASE("SidebarTree.filter_matches_daemon_names", "[sidebar]")
{
    auto star = sectionOf(daemonB(), 1, pairOf());
    star.name = "starfive";
    auto const sections = std::vector { sectionOf(daemonA(), 1, pairOf()), star };
    auto state = openedOn(daemonA());
    state.filter = "STAR";
    // A daemon whose name matches keeps its workspaces, folded.
    auto const byName = listingOf(sections, state).rows;
    CHECK(describe(byName) == std::vector<std::string> { "D host:b", "W host:b 1", "W host:b 2" });
    CHECK(rowAt(byName, "W host:b 1").expansion == RowExpansion::Collapsed);
    CHECK(rowAt(byName, "D host:b").expansion == RowExpansion::Expanded);

    // Across daemons, a tab's name matches on each of them.
    state.filter = "t2";
    CHECK(describe(listingOf(sections, state).rows)
          == std::vector<std::string> {
              "D local:a", "W local:a 2", "T local:a 2 1", "D host:b", "W host:b 2", "T host:b 2 1" });
}

TEST_CASE("SidebarTree.pending_target_marks_its_row", "[sidebar]")
{
    auto const sections =
        std::vector { sectionOf(daemonA(), 1, pairOf()), sectionOf(daemonB(), 1, pairOf()) };
    auto state = openedOn(daemonA());
    state.toggled = { workspaceKeyOn(daemonB(), 1, 1) };
    auto const rows = listingOf(sections, state, std::nullopt, targetOf(daemonB(), 1, 1)).rows;
    CHECK(rowAt(rows, "T host:b 1 1").pending == RowPending::Yes);
    CHECK(rowAt(rows, "T host:b 1 1").slot == StatusSlot::Pending);
    CHECK(rowAt(rows, "D host:b").pending == RowPending::Yes);
    CHECK(rowAt(rows, "D host:b").slot == StatusSlot::Pending);
    CHECK(rowAt(rows, "W host:b 1").pending == RowPending::No);
    CHECK(std::ranges::count(rows, RowPending::Yes, &SidebarRow::pending) == 2);

    // Folded away, the target's nearest row shown carries it.
    auto const folded =
        listingOf(sections, openedOn(daemonA()), std::nullopt, targetOf(daemonB(), 1, 1)).rows;
    CHECK(rowAt(folded, "W host:b 1").slot == StatusSlot::Pending);
}

TEST_CASE("SidebarTree.reconnecting_daemon_keeps_last_known_tree", "[sidebar]")
{
    // Every state but Connected: a section's directory is then what was last known of the daemon, whether
    // its link is being tried, waits for the user, or the daemon has stopped.
    for (auto const down: { DaemonLinkState::NotConnected,
                            DaemonLinkState::Connecting,
                            DaemonLinkState::Reconnecting,
                            DaemonLinkState::NeedsInput,
                            DaemonLinkState::NotRunning,
                            DaemonLinkState::TooOld,
                            DaemonLinkState::Failed })
    {
        INFO("state " << static_cast<int>(down));
        auto const rows =
            listingOf({ sectionOf(daemonA(), 1, pairOf()), sectionOf(daemonB(), 1, pairOf(), down) },
                      openedOn(daemonB()))
                .rows;
        CHECK(rowAt(rows, "T host:b 2 1").title == "t2");
        CHECK(rowAt(rows, "D host:b").expansion == RowExpansion::Expanded);
        for (auto const& row: rows)
            CHECK(row.staleness
                  == (row.key.daemon == daemonB() ? RowStaleness::LastKnown : RowStaleness::Live));
    }
    // Nothing known: nothing stale, and nothing to fold.
    auto const empty =
        listingOf({ sectionOf(daemonB(), 1, {}, DaemonLinkState::Reconnecting) }, openedOn(daemonB())).rows;
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].staleness == RowStaleness::Live);
    CHECK(empty[0].expansion == RowExpansion::None);
}

TEST_CASE("SidebarTree.last_known_rows_claim_nothing_live", "[sidebar]")
{
    // b stopped while a pane of it worked and another waited for the user: what was last known draws no
    // running mark and is counted neither by the footer nor by the chip's roll-up.
    auto const sections = std::vector { sectionOf(daemonA(), 1, pairOf()),
                                        sectionOf(daemonB(),
                                                  1,
                                                  pairOf(ProgramState::Working, ProgramState::Blocked),
                                                  DaemonLinkState::NotRunning) };
    auto const listing = listingOf(sections, openedOn(daemonB()));
    auto const& working = rowAt(listing.rows, "T host:b 1 1");
    CHECK(working.staleness == RowStaleness::LastKnown);
    CHECK(working.glyph == TabBadgeKind::None);
    CHECK(working.time.kind != TimeKind::Live);
    CHECK(working.time.kind != TimeKind::Progress);
    // What asks for the user keeps its mark: the row says what was last known of it.
    CHECK(rowAt(listing.rows, "T host:b 2 1").glyph == TabBadgeKind::Blocked);
    CHECK(sidebarFooter(listing.workspaces) == SidebarFooter {});
    auto const summary = sidebarSummary(listing.workspaces, workspaceKeyOn(daemonA(), 1, 1));
    CHECK(summary.otherWorkspaces == Urgency::Idle);
    CHECK(chipMarkerOf(summary.otherWorkspaces) == ChipMarker::None);
    // A last-known workspace still names the window's own, whose link is down.
    CHECK(sidebarSummary(listing.workspaces, workspaceKeyOn(daemonB(), 1, 2)).currentWorkspace == "two");

    // Once b is back, the same panes count.
    auto live = sections;
    live[1].state = DaemonLinkState::Connected;
    auto const back = listingOf(live, openedOn(daemonB()));
    CHECK(rowAt(back.rows, "T host:b 1 1").glyph == TabBadgeKind::Working);
    CHECK(sidebarFooter(back.workspaces).needYou == 1);
    CHECK(sidebarFooter(back.workspaces).working == 1);
    CHECK(sidebarSummary(back.workspaces, workspaceKeyOn(daemonA(), 1, 1)).otherWorkspaces
          == Urgency::NeedsYou);
}

TEST_CASE("SidebarTree.first_match_prefers_a_row_that_itself_matches", "[sidebar]")
{
    // A pane "logs" under a tab "t1" that does not match: the tab is listed as the pane's ancestor, but
    // Return goes to the pane.
    auto directory =
        directoryOf({ workspaceOf(1,
                                  "one",
                                  { tabOf(1,
                                          "t1",
                                          { paneOf(1, "shell", { recordOf(ProgramState::Idle) }),
                                            paneOf(2, "logs", { recordOf(ProgramState::Idle) }) }) }) });
    auto state = openedOn(daemonA());
    state.filter = "logs";
    auto const rows = listingOf({ sectionOf(daemonA(), 1, directory) }, state).rows;
    auto const first = firstMatchOf(rows, state);
    REQUIRE(first.has_value());
    CHECK(rows[*first].key.kind == SidebarRowKind::Pane);
    CHECK(rows[*first].title == "logs");

    // A workspace that matches by name, with no tab that does, is the match.
    state.filter = "one";
    auto const byWorkspace = listingOf({ sectionOf(daemonA(), 1, directory) }, state).rows;
    auto const workspace = firstMatchOf(byWorkspace, state);
    REQUIRE(workspace.has_value());
    CHECK(byWorkspace[*workspace].key.kind == SidebarRowKind::Workspace);

    // Nothing typed and every row listed: nothing narrows the tree, and Return has no match to go to.
    CHECK(isUnnarrowed(openedOn(daemonA())));
    CHECK_FALSE(isUnnarrowed(state));
    auto needsMe = openedOn(daemonA());
    needsMe.scope = contour::config::SidebarScope::NeedsMe;
    CHECK_FALSE(isUnnarrowed(needsMe));
}

TEST_CASE("SidebarTree.shown_elsewhere_marks_workspace", "[sidebar]")
{
    auto b = sectionOf(daemonB(), 1, pairOf());
    b.shownElsewhere = { 2 };
    auto const rows = listingOf({ sectionOf(daemonA(), 1, pairOf()), b }, openedOn(daemonA())).rows;
    CHECK(rowAt(rows, "W host:b 2").shownElsewhere == ShownElsewhere::Yes);
    CHECK(rowAt(rows, "W host:b 1").shownElsewhere == ShownElsewhere::No);
    CHECK(rowAt(rows, "W local:a 2").shownElsewhere == ShownElsewhere::No);
}

TEST_CASE("SidebarTree.hidden_row_lists_hidden_daemons", "[sidebar]")
{
    auto const hidden = std::vector { HiddenDaemon { .daemon = hostDaemonRef("c"), .name = "c" },
                                      HiddenDaemon { .daemon = hostDaemonRef("d"), .name = "d" } };
    auto const sections = std::vector { sectionOf(daemonA(), 1, pairOf()) };
    auto const rows = listingOf(sections, openedOn(daemonA()), std::nullopt, std::nullopt, hidden).rows;
    REQUIRE(!rows.empty());
    CHECK(rows.back().key.kind == SidebarRowKind::Hidden);
    CHECK(rows.back().title.empty());
    CHECK(rows.back().hidden == hidden);
    CHECK(std::ranges::none_of(listingOf(sections, openedOn(daemonA())).rows,
                               [](SidebarRow const& row) { return row.key.kind == SidebarRowKind::Hidden; }));
}

TEST_CASE("SidebarTree.footer_and_summary_span_daemons", "[sidebar]")
{
    auto const listing =
        listingOf({ sectionOf(daemonA(), 1, pairOf(ProgramState::Blocked, ProgramState::Working)),
                    sectionOf(daemonB(), 1, pairOf(ProgramState::Working, ProgramState::Error)) },
                  openedOn(daemonA()));
    REQUIRE(listing.workspaces.size() == 4);
    CHECK(sidebarFooter(listing.workspaces) == SidebarFooter { .needYou = 1, .working = 2 });
    // b's folded rows count as much as a's.
    CHECK(sidebarFooter(listing.rows) == sidebarFooter(listing.workspaces));

    auto const onA = sidebarSummary(listing.workspaces, workspaceKeyOn(daemonA(), 1, 1));
    CHECK(onA.currentWorkspace == "one");
    CHECK(onA.otherWorkspaces == Urgency::Failed);
    // Workspace 1 on b is not workspace 1 on a.
    auto const onB = sidebarSummary(listing.workspaces, workspaceKeyOn(daemonB(), 1, 1));
    CHECK(onB.currentWorkspace == "one");
    CHECK(onB.otherWorkspaces == Urgency::NeedsYou);
}

TEST_CASE("SidebarTree.terminal_row_widths_unchanged_at_18_cells", "[sidebar]")
{
    auto split = pairOf();
    split.workspaces[0].tabs.push_back(tabOf(2, "split", { paneOf(11, "a"), paneOf(12, "b") }));
    auto const sections = std::vector { sectionOf(daemonA(), 1, split), sectionOf(daemonB(), 1, split) };
    auto state = SidebarState {};
    state.openedOn = localDaemonRef("elsewhere");
    state.toggled = { workspaceKeyOn(daemonA(), 1, 1),
                      workspaceKeyOn(daemonA(), 1, 2),
                      workspaceKeyOn(daemonB(), 1, 1),
                      workspaceKeyOn(daemonB(), 1, 2) };
    auto const unfolded = listingOf(sections, state).rows;
    auto const single = rowsOf(split);
    for (auto const& row: unfolded)
    {
        if (row.depth != 0)
            continue;
        CHECK(treeLinesText(row.lines, row.depth).empty());
        CHECK(treeContinuationText(row).empty());
    }
    // Each section's rows below its heading draw what the daemonless tree draws, cell for cell.
    auto const textsOf = [](std::span<SidebarRow const> shown) {
        return std::ranges::to<std::vector>(shown | std::views::transform([](SidebarRow const& row) {
                                                return treeLinesText(row.lines, row.depth) + "|"
                                                       + treeContinuationText(row);
                                            }));
    };
    REQUIRE(unfolded.size() == (single.size() + 1) * 2);
    CHECK(textsOf(std::span { unfolded }.subspan(1, single.size())) == textsOf(single));
    CHECK(textsOf(std::span { unfolded }.subspan(single.size() + 2)) == textsOf(single));
}

TEST_CASE("SidebarTree.workspace_rail_cells_unchanged", "[sidebar]")
{
    auto split = pairOf();
    split.workspaces[0].tabs.push_back(tabOf(2, "split", { paneOf(11, "a"), paneOf(12, "b") }));
    split.workspaces.push_back(workspaceOf(3, "empty", {}));
    auto const rows = listingOf({ sectionOf(daemonA(), 1, split) }, openedOn(daemonA())).rows;
    auto const single = rowsOf(split);
    REQUIRE(rows.size() == single.size() + 1);
    CHECK(workspaceRailCellsOf(rows[0]) == RailCells {});
    auto const cellsOf = [](std::span<SidebarRow const> shown) {
        return std::ranges::to<std::vector>(shown | std::views::transform(workspaceRailCellsOf));
    };
    CHECK(cellsOf(std::span { rows }.subspan(1)) == cellsOf(single));
}

TEST_CASE("SidebarTree.window_without_daemon_has_no_daemon_row", "[sidebar]")
{
    auto const rows = rowsOf(pairOf());
    REQUIRE(rows.size() == 4);
    CHECK(std::ranges::none_of(rows,
                               [](SidebarRow const& row) { return row.key.kind == SidebarRowKind::Daemon; }));
    CHECK(std::ranges::all_of(rows, [](SidebarRow const& row) { return row.key.daemon == DaemonRef {}; }));
    CHECK(std::ranges::all_of(rows, [](SidebarRow const& row) { return row.slot != StatusSlot::Link; }));
}

TEST_CASE("SidebarTree.left_from_a_workspace_reaches_its_daemon_row", "[sidebar]")
{
    auto state = openedOn(daemonA());
    state.toggled = { workspaceKeyOn(daemonB(), 1, 1) };
    auto const rows = listingOf({ sectionOf(daemonA(), 1, pairOf()), sectionOf(daemonB(), 1, pairOf()) },
                                state,
                                std::nullopt,
                                std::nullopt,
                                { HiddenDaemon { .daemon = hostDaemonRef("c"), .name = "c" } })
                          .rows;
    auto const daemonRowA = indexOf(rows, "D local:a");
    auto const daemonRowB = indexOf(rows, "D host:b");
    CHECK(parentRowOf(rows, indexOf(rows, "W local:a 1")) == daemonRowA);
    CHECK(parentRowOf(rows, indexOf(rows, "W local:a 2")) == daemonRowA);
    CHECK(parentRowOf(rows, indexOf(rows, "W host:b 2")) == daemonRowB);
    CHECK(parentRowOf(rows, indexOf(rows, "T host:b 1 1")) == indexOf(rows, "W host:b 1"));
    CHECK(parentRowOf(rows, daemonRowB) == -1);
    CHECK(parentRowOf(rows, indexOf(rows, "H")) == -1);
}
