// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarTree.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <initializer_list>
#include <optional>
#include <ranges>
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

DirectoryTab tabOf(uint64_t id, std::string label, std::vector<DirectoryPane> panes)
{
    auto tab = DirectoryTab {};
    tab.id = id;
    tab.label = std::move(label);
    tab.panes = std::move(panes);
    return tab;
}

DirectoryWorkspace workspaceOf(uint64_t id, std::string label, std::vector<DirectoryTab> tabs)
{
    auto workspace = DirectoryWorkspace {};
    workspace.id = id;
    workspace.label = std::move(label);
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
    CHECK(rows[1].key == SidebarRowKey { SidebarRowKind::Tab, 7, 3, PaneKey {} });
    CHECK(rows[1].expansion == RowExpansion::Expanded);
    CHECK(rows[2].key == SidebarRowKey { SidebarRowKind::Pane, 7, 3, PaneKey { 10 } });
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
                                 SidebarState { .collapsedWorkspaces = { 1 } });
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
    CHECK(rowsOf(directory, SidebarState { .collapsedWorkspaces = { 1 } }).size() == 2);
    auto const tabFolded = rowsOf(directory, SidebarState { .collapsedTabs = { TabRef { 1, 1 } } });
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
                   .collapsedWorkspaces = { 1 }, .collapsedTabs = { TabRef { 1, 1 } }, .filter = "make" });
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
    CHECK(sidebarFooter(rowsOf(directory, SidebarState { .collapsedWorkspaces = { 1, 2 } })) == expected);
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
    CHECK(sidebarSummary(listing.workspaces, 2) == sidebarSummary(rowsOf(directory), 2));
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
    auto const folded = rowsOf(directory, SidebarState { .collapsedWorkspaces = { 2 } }, TabRef { 2, 1 });
    CHECK(std::ranges::to<std::vector>(folded | std::views::transform(&SidebarRow::selection))
          == std::vector { None, None, None, ContainsSelected });
}

TEST_CASE("SidebarTree.collapsing_a_tab_does_not_collapse_its_namesake", "[sidebar]")
{
    auto const directory = directoryOf({ workspaceOf(1, "a", { tabOf(1, "t", { paneOf(1), paneOf(2) }) }),
                                         workspaceOf(2, "b", { tabOf(1, "u", { paneOf(3), paneOf(4) }) }) });
    auto const rows = rowsOf(directory, SidebarState { .collapsedTabs = { TabRef { 2, 1 } } });
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
    CHECK(positions(rowsOf(directory, SidebarState { .collapsedWorkspaces = { 5 } }))
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
    // vthost::proto::FinishedCommand carries no timestamp, so nothing can be measured from.
    auto pane = paneOf(1, "p");
    pane.finished = vthost::proto::FinishedCommand { .exitCode = 1, .durationMs = 18'000 };
    pane.finishedSerial = 1;
    auto const row = tabRowOf(rowsOf(directoryOf({ workspaceOf(1, "w", { tabOf(1, "t", { pane }) }) })));
    CHECK(row.reason == "exit 1 · 18s");
    CHECK(row.time == TimeText {});
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

    auto const onDocs = sidebarSummary(rows, 2);
    CHECK(onDocs.currentWorkspace == "docs");
    CHECK(onDocs.otherWorkspaces == Urgency::NeedsYou);
    CHECK(onDocs.otherWorkspacesGlyph == rows.front().glyph);
    CHECK(chipMarkerOf(onDocs.otherWorkspaces) == ChipMarker::Shown);

    // Work running elsewhere does not mark the chip.
    auto const onBilling = sidebarSummary(rows, 1);
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

TEST_CASE("SidebarTree.meter_text_fills_eighths_of_cells", "[sidebar]")
{
    CHECK(meterText(0) == "░░░░░");
    CHECK(meterText(100) == "█████");
    CHECK(meterText(50) == "██▌░░");
    CHECK(meterText(-5) == meterText(0));
    CHECK(meterText(250) == meterText(100));
}

TEST_CASE("SidebarTree.the_chip_names_what_the_other_workspaces_ask", "[sidebar]")
{
    auto summary = SidebarSummary { .currentWorkspace = "docs", .otherWorkspaces = Urgency::Failed };
    CHECK(workspaceChipAccessibleName(summary) == "Workspace docs, another workspace failed");
    summary.otherWorkspaces = Urgency::DoneUnseen;
    CHECK(workspaceChipAccessibleName(summary) == "Workspace docs, another workspace done");
    summary.otherWorkspaces = Urgency::NeedsYou;
    CHECK(workspaceChipAccessibleName(summary) == "Workspace docs, another workspace needs you");
    // What does not mark the chip is not said either.
    summary.otherWorkspaces = Urgency::Working;
    CHECK(workspaceChipAccessibleName(summary) == "Workspace docs");
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

TEST_CASE("every kind of sidebar row hands the keyboard to the terminal once activated", "[sidebar]")
{
    for (auto const kind: { SidebarRowKind::Workspace, SidebarRowKind::Tab, SidebarRowKind::Pane })
        CHECK(focusAfterActivation(kind) == ActivationFocus::Terminal);
}
