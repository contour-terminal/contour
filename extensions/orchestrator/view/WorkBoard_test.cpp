// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/Actions.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/ViewFixtures.hpp>
#include <orchestrator/view/WorkBoard.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;
using core::tui::Size;

namespace
{

constexpr auto Wide = Size { .width = 120, .height = 40 };
constexpr auto Medium = Size { .width = 80, .height = 24 };

/// What the Work view lists of an engine, as the dashboard builds it.
struct BoardLines
{
    explicit BoardLines(Engine const& engine, WorkState const& state = {}):
        items { engine.workItems() },
        order { orderOf(items, engine.repositories(), sortOf(engine)) },
        lines { workLines(items, order, engine, state) }
    {
    }

    /// @return Each line as the tests name it: a heading by its group's name, an item by its MR or key.
    [[nodiscard]] std::vector<std::string> names() const
    {
        auto named = std::vector<std::string> {};
        for (auto const& line: lines)
        {
            if (line.kind == WorkLineKind::Heading)
            {
                named.emplace_back(WorkGroupRows[std::to_underlying(line.group)].name);
                continue;
            }
            auto const& item = items[line.item];
            named.push_back(item.changeRequest ? std::format("#{}", item.changeRequest->number)
                                               : item.ticketKey);
        }
        return named;
    }

    std::vector<WorkItem> items;
    std::vector<std::size_t> order;
    std::vector<WorkLine> lines;
};

/// @return The lines of @p names that follow @p heading, up to the next heading.
[[nodiscard]] std::vector<std::string> groupOf(BoardLines const& board, std::string_view heading)
{
    auto const names = board.names();
    auto group = std::vector<std::string> {};
    auto inside = false;
    for (auto const [at, line]: std::views::enumerate(board.lines))
    {
        if (line.kind == WorkLineKind::Heading)
        {
            inside = names[static_cast<std::size_t>(at)] == heading;
            continue;
        }
        if (inside)
            group.push_back(names[static_cast<std::size_t>(at)]);
    }
    return group;
}

/// @return The Work view's selected line's name, from the dashboard of @p fixture.
[[nodiscard]] std::string selectedName(DashboardFixture const& fixture)
{
    auto const board = BoardLines { fixture.engine, fixture.dashboard.interaction().work };
    auto const& work = fixture.dashboard.interaction().work;
    REQUIRE(work.selected < board.lines.size());
    return board.names()[work.selected];
}

} // namespace

TEST_CASE("Work view: w switches views and back", "[orchestrator][view][work]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.input("w").empty());
    CHECK(fixture.dashboard.interaction().view == View::Work);
    CHECK(fixture.screen().contains("Needs you"));
    CHECK(fixture.dashboard.input("w").empty());
    CHECK(fixture.dashboard.interaction().view == View::Agents);

    // In the Agents view tab still moves between the Fleet and the Focus.
    CHECK(fixture.dashboard.select(WorkRunId).empty());
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(fixture.dashboard.interaction().place == Place::Focus);
    CHECK(fixture.dashboard.input("\t").empty());
    CHECK(fixture.dashboard.interaction().place == Place::Fleet);

    // In the Work view it moves along the list and the filter bar's chips.
    CHECK(fixture.dashboard.input("w").empty());
    auto focuses = std::vector<WorkFocus> {};
    for ([[maybe_unused]] auto const step: std::views::iota(0, 4))
    {
        CHECK(fixture.dashboard.input("\t").empty());
        focuses.push_back(fixture.dashboard.interaction().work.focus);
    }
    CHECK(focuses
          == std::vector { WorkFocus::Filter, WorkFocus::RepoChip, WorkFocus::SortChip, WorkFocus::List });
}

TEST_CASE("Work view: groups in order, Everything else folded", "[orchestrator][view][work]")
{
    auto const engine = bootEngine(workWorld());
    auto const board = BoardLines { engine };
    auto headings = std::vector<std::string> {};
    for (auto const& line: board.lines)
        if (line.kind == WorkLineKind::Heading)
            headings.emplace_back(WorkGroupRows[std::to_underlying(line.group)].name);
    CHECK(headings
          == std::vector<std::string> {
              "Needs you", "Ready to merge", "In review", "No MR yet", "Everything else" });
    CHECK(board.lines.back().kind == WorkLineKind::Heading);
    CHECK(board.lines.back().group == WorkGroup::EverythingElse);
    CHECK(groupOf(board, "Needs you") == std::vector<std::string> { "#2375", "#2376", "#2377" });
    CHECK(groupOf(board, "Ready to merge") == std::vector<std::string> { "#2378" });
    CHECK(groupOf(board, "In review") == std::vector<std::string> { "#2379" });
    CHECK(std::ranges::contains(groupOf(board, "No MR yet"), "PRJ-1302"s));

    // #2379 names PRJ-1999, which no source listed: the key lookup's status shows on its row.
    auto fixture = DashboardFixture { workWorld(), Wide };
    CHECK(fixture.dashboard.input("w").empty());
    auto const screen = fixture.screen();
    auto const lines =
        screen | std::views::split('\n')
        | std::views::transform([](auto const& line) { return std::string(std::from_range, line); })
        | std::ranges::to<std::vector>();
    auto const row =
        std::ranges::find_if(lines, [](std::string const& line) { return line.contains("#2379"); });
    REQUIRE(row != lines.end());
    CHECK(row->contains("◆ In Review"));
}

TEST_CASE("Work view: z folds the selected row's group and the fold persists", "[orchestrator][view][work]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    CHECK(fixture.dashboard.input("w").empty());
    static_cast<void>(fixture.screen());
    // The first item is selected: Needs you's newest.
    CHECK(selectedName(fixture) == "#2375");
    auto const folding = fixture.dashboard.input("z");
    CHECK(folding
          == std::vector<UiAction> {
              SetFoldAction { .group = "work/needs-you", .choice = FoldChoice::Folded } });
    static_cast<void>(fixture.carryOut(folding));
    CHECK(isGroupFolded(fixture.engine, WorkGroup::NeedsYou));
    static_cast<void>(fixture.screen());
    CHECK(selectedName(fixture) == "Needs you");
    CHECK(fixture.screen().contains("▸ Needs you  3"));

    // ⏎ on Everything else's heading unfolds it.
    CHECK(fixture.dashboard.input("\x1b[F").empty());
    CHECK(selectedName(fixture) == "Everything else");
    auto const unfolding = fixture.dashboard.input("\r");
    CHECK(unfolding
          == std::vector<UiAction> { SetFoldAction { .group = "work/else", .choice = FoldChoice::Open } });
    static_cast<void>(fixture.carryOut(unfolding));
    CHECK_FALSE(isGroupFolded(fixture.engine, WorkGroup::EverythingElse));
    CHECK(std::ranges::contains(groupOf(BoardLines { fixture.engine }, "Everything else"), "#2380"s));
}

TEST_CASE("Work view: s switches to one flat list by last update, persisted", "[orchestrator][view][work]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    CHECK(fixture.dashboard.input("w").empty());
    auto const sorting = fixture.dashboard.input("s");
    CHECK(sorting == std::vector<UiAction> { ToggleFoldAction { .group = std::string { SortByUpdateKey } } });
    static_cast<void>(fixture.carryOut(sorting));
    CHECK(sortOf(fixture.engine) == WorkSort::LastUpdate);
    auto const board = BoardLines { fixture.engine };
    CHECK(std::ranges::none_of(board.lines,
                               [](WorkLine const& line) { return line.kind == WorkLineKind::Heading; }));
    auto const names = board.names();
    REQUIRE(names.size() >= 6);
    CHECK(std::vector(names.begin(), names.begin() + 6)
          == std::vector<std::string> { "#2375", "#2376", "#2377", "#2378", "#2379", "#2380" });
    CHECK(fixture.screen().contains("sort: last update"));
}

TEST_CASE("Work view: the filter, the repository chip and Esc", "[orchestrator][view][work]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    CHECK(fixture.dashboard.input("w/2375").empty());
    auto const& work = fixture.dashboard.interaction().work;
    CHECK(work.filter == "2375");
    CHECK(BoardLines { fixture.engine, work }.names() == std::vector<std::string> { "Needs you", "#2375" });
    // The first esc stops typing and keeps the filter; the second clears it.
    CHECK(fixture.dashboard.input("\x1b").empty());
    static_cast<void>(fixture.escapeTimesOut());
    CHECK(work.filter == "2375");
    CHECK(work.filtering == Filtering::Off);
    CHECK(fixture.dashboard.input("\x1b").empty());
    static_cast<void>(fixture.escapeTimesOut());
    CHECK(work.filter.empty());
    CHECK(work.filtering == Filtering::Off);
    CHECK(BoardLines { fixture.engine, work }.names().size() > 2);
    CHECK(fixture.dashboard.input("/prj-1301").empty());
    CHECK(BoardLines { fixture.engine, work }.names() == std::vector<std::string> { "Needs you", "#2377" });

    SECTION("the repository chip steps through the repositories and back to all")
    {
        auto world = workWorld();
        world.repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github" },
                           { "name": "other", "path": "~/projects/other", "forge": "github" } ])";
        world.repoPaths = { std::string { ContourPath }, std::string { OtherPath } };
        auto two = DashboardFixture { world, Wide };
        CHECK(two.dashboard.input("w").empty());
        auto const all = BoardLines { two.engine, two.dashboard.interaction().work }.items.size();
        CHECK(two.dashboard.input("\x1b[C").empty());
        CHECK(two.dashboard.interaction().work.repoChip == 1);
        CHECK(two.screen().contains("‹ contour ›"));
        auto const& chipped = two.dashboard.interaction().work;
        auto const board = BoardLines { two.engine, chipped };
        CHECK(std::ranges::all_of(board.lines, [&](WorkLine const& line) {
            return line.kind == WorkLineKind::Heading || board.items[line.item].repo == "contour";
        }));
        CHECK(two.dashboard.input("\x1b[D").empty());
        CHECK(two.dashboard.interaction().work.repoChip == 0);
        CHECK(two.screen().contains("‹ all repositories ›"));
        CHECK(all > 0);
    }
}

TEST_CASE("Work view: the selection stays in view while it moves and the list scrolls",
          "[orchestrator][view][work]")
{
    auto world = workWorld();
    world.prs = { .text = manyPrs(60) };
    world.issues = { .text = "[]" };
    world.answers.clear();
    world.repos = std::string { OneRepository };
    auto fixture = DashboardFixture { world, Medium };
    CHECK(fixture.dashboard.input("w").empty());
    static_cast<void>(fixture.screen());
    auto const& work = fixture.dashboard.interaction().work;
    // A heading and the sixty pull requests: more than the 18 lines the list shows at 80x24.
    constexpr auto Lines = std::size_t { 61 };
    constexpr auto Rows = std::size_t { 18 };
    CHECK(BoardLines { fixture.engine, work }.lines.size() == Lines);

    CHECK(fixture.dashboard.input("\x1b[F").empty());
    CHECK(work.selected == Lines - 1);
    CHECK(work.scroll == Lines - Rows);
    auto const screen = fixture.screen();
    CHECK(screen.contains("title 60"));
    CHECK_FALSE(screen.contains("title 1 "));

    // A page up moves by the lines shown less one, and the list follows the selection.
    CHECK(fixture.dashboard.input("\x1b[5~").empty());
    CHECK(work.selected == Lines - 1 - (Rows - 1));
    CHECK(work.scroll <= work.selected);
    CHECK(fixture.dashboard.input("\x1b[5~\x1b[5~\x1b[5~").empty());
    CHECK(work.selected == 0);
    CHECK(work.scroll == 0);
    CHECK(fixture.dashboard.input("\x1b[6~").empty());
    CHECK(work.selected == Rows - 1);
    CHECK(work.scroll == 0);
    CHECK(fixture.dashboard.input("j").empty());
    CHECK(work.scroll == 1);
}

TEST_CASE("Work view: a long list draws only its visible lines", "[orchestrator][view][work]")
{
    auto world = workWorld();
    world.prs = { "github/prs-100.json" };
    auto fixture = DashboardFixture { world, Wide };
    CHECK(fixture.dashboard.input("w").empty());
    auto const screen = fixture.screen();
    CHECK(screen.contains("Work "));
    auto const builds = fixture.dashboard.rowBuilds();
    // Frames, keys and a nudge that changes nothing in the engine leave the items as they were built.
    for ([[maybe_unused]] auto const frame: std::views::iota(0, 5))
    {
        fixture.dashboard.engineChanged();
        static_cast<void>(fixture.screen());
        CHECK(fixture.dashboard.input("j").empty());
    }
    CHECK(fixture.dashboard.rowBuilds() == builds);
    // A fold changes the engine's revision: the items are built again.
    static_cast<void>(fixture.carryOut(fixture.dashboard.input("s")));
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.rowBuilds() == builds + 1);
}

TEST_CASE("Work view: o opens the selected MR or ticket", "[orchestrator][view][work]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    CHECK(fixture.dashboard.input("w").empty());
    static_cast<void>(fixture.screen());
    CHECK(selectedName(fixture) == "#2375");
    CHECK(fixture.dashboard.input("o")
          == std::vector<UiAction> {
              OpenUrlAction { .url = "https://github.com/example/contour/pull/2375", .label = "#2375" } });
    CHECK(fixture.dashboard.input("/prj-1302\r").empty());
    CHECK(selectedName(fixture) == "PRJ-1302");
    CHECK(fixture.dashboard.input("o")
          == std::vector<UiAction> { OpenUrlAction { .url = "https://example.atlassian.net/browse/PRJ-1302",
                                                     .label = "PRJ-1302" } });
}

TEST_CASE("Work view: the frame asks for any-motion reporting while it is shown",
          "[orchestrator][view][work]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    CHECK_FALSE(fixture.dashboard.frame().contains(AnyMotionOn));
    CHECK(fixture.dashboard.input("w").empty());
    CHECK(fixture.dashboard.frame().contains(AnyMotionOn));
    CHECK(fixture.dashboard.input("w").empty());
    auto const leaving = fixture.dashboard.frame();
    CHECK(leaving.contains(AnyMotionOff));
    CHECK_FALSE(leaving.contains(AnyMotionOn));
    CHECK(fixture.dashboard.input("j").empty());
    CHECK_FALSE(fixture.dashboard.frame().contains(AnyMotionOff));
}

TEST_CASE("Work view: an update's age", "[orchestrator][view][work]")
{
    CHECK(updateAgeText(45'000) == "45s");
    CHECK(updateAgeText((12 * 60'000) + 59'000) == "12m");
    CHECK(updateAgeText((3 * 3'600'000) + 1) == "3h");
    CHECK(updateAgeText((uint64_t { 2 } * 86'400'000) + 3'600'000) == "2d");
}

TEST_CASE("Work view: an age changes once the rest of its unit passed", "[orchestrator][view][work]")
{
    CHECK(untilAgeChanges(45'400) == 600);
    CHECK(untilAgeChanges(uint64_t { 12 } * 60'000) == 60'000);
    CHECK(untilAgeChanges((3 * 3'600'000) + 1) == 3'599'999);
    CHECK(untilAgeChanges(uint64_t { 2 } * 86'400'000) == 86'400'000);
}

TEST_CASE("Work view: the hidden Fleet and Focus ask for no wake and read nothing",
          "[orchestrator][view][work]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    static_cast<void>(fixture.screen());
    // The running run is in the Focus, its transcript read and its age counting in its row.
    CHECK(fixture.dashboard.select(WorkRunId).empty());
    static_cast<void>(fixture.settle());
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.input("w").empty());
    static_cast<void>(fixture.settle());
    static_cast<void>(fixture.screen());

    // Past the Fleet row's next change and the transcript's next read: nothing of them is due.
    fixture.clock.advance(std::chrono::minutes { 2 });
    CHECK(fixture.settle().empty());
    static_cast<void>(fixture.dashboard.frame());
    auto const due = fixture.dashboard.nextRepaintDue();
    REQUIRE(due.has_value());
    CHECK(*due > fixture.clock.now());
    CHECK(fixture.dashboard.frame().empty());

    // The next due time is the Work view's own: the first age it draws that moves on, and it does move on.
    auto const before = fixture.screen();
    CHECK(before.contains("3h"));
    fixture.clock.advance(*due - fixture.clock.now());
    CHECK_FALSE(fixture.dashboard.frame().empty());
    CHECK(*fixture.dashboard.nextRepaintDue() > fixture.clock.now());
    CHECK(fixture.screen() != before);

    // Back in the Agents view, the Fleet's timers count again.
    CHECK(fixture.dashboard.input("w").empty());
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.nextRepaintDue().has_value());
}

TEST_CASE("Work view: the configured folds, the user's choice, a fold stored before",
          "[orchestrator][view][work]")
{
    auto const configured = [](std::string_view work) {
        auto world = workWorld();
        world.work = std::string { work };
        return world;
    };

    // The default configuration: Everything else folded, the rest open.
    auto const defaults = bootEngine(workWorld());
    CHECK(isGroupFolded(defaults, WorkGroup::EverythingElse));
    CHECK_FALSE(isGroupFolded(defaults, WorkGroup::NeedsYou));

    // `work.folded` says which.
    auto inReview = bootEngine(configured(R"({ "folded": ["in_review"] })"));
    CHECK(isGroupFolded(inReview, WorkGroup::InReview));
    CHECK_FALSE(isGroupFolded(inReview, WorkGroup::EverythingElse));

    // A toggle stored before choices flips the row's built-in default: Everything else stays open, as the
    // user left it.
    static_cast<void>(inReview.step(ToggleFold { .group = "work/else", .monoMs = StartMonoMs }));
    CHECK_FALSE(isGroupFolded(inReview, WorkGroup::EverythingElse));

    // The user's choice wins over `work.folded`, whatever it says.
    for (auto const work: { R"({ "folded": ["in_review"] })"sv, R"({ "folded": [] })"sv })
    {
        CAPTURE(work);
        auto engine = bootEngine(configured(work));
        static_cast<void>(engine.step(
            SetFold { .group = "work/in-review", .choice = FoldChoice::Open, .monoMs = StartMonoMs }));
        CHECK_FALSE(isGroupFolded(engine, WorkGroup::InReview));
    }

    // Folding a folded group asks to open it.
    auto state = WorkState {};
    CHECK(foldWorkGroup(state, inReview, WorkGroup::InReview)
          == UiAction { SetFoldAction { .group = "work/in-review", .choice = FoldChoice::Open } });
    CHECK(state.selectedKey == "group:work/in-review");
    CHECK(foldWorkGroup(state, inReview, WorkGroup::NeedsYou)
          == UiAction { SetFoldAction { .group = "work/needs-you", .choice = FoldChoice::Folded } });
}

TEST_CASE("Work view: fold_empty false shows every group's heading", "[orchestrator][view][work]")
{
    // One pull request whose pipeline fails, and nothing else: every item is Needs you's.
    auto const needsYouOnly = [](std::string_view work) {
        auto world = World {
            .prs = { .text = R"([[2375,"fix the flaky test","https://github.com/example/contour/pull/2375",)"
                             R"("fix/flaky-test","master",false,false,"",[],[["CheckRun","test",null,)"
                             R"("COMPLETED","FAILURE",null]],"MERGEABLE","2026-11-13T12:00:00Z"]])" },
            .issues = { .text = "[]" },
        };
        world.work = std::string { work };
        return world;
    };
    auto const headings = [](Engine const& engine) {
        auto const board = BoardLines { engine };
        return std::ranges::count(board.lines, WorkLineKind::Heading, &WorkLine::kind);
    };

    auto const hidden = bootEngine(needsYouOnly({}));
    REQUIRE_FALSE(hidden.workItems().empty());
    REQUIRE(std::ranges::all_of(hidden.workItems(),
                                [](WorkItem const& item) { return item.group == WorkGroup::NeedsYou; }));
    CHECK(headings(hidden) == 1);
    CHECK(headings(bootEngine(needsYouOnly(R"({ "fold_empty": false })"))) == 5);
}

TEST_CASE("Work view: h and l step the repository chip", "[orchestrator][view][work]")
{
    auto world = workWorld();
    world.repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github" },
                       { "name": "other", "path": "~/projects/other", "forge": "github" } ])";
    world.repoPaths = { std::string { ContourPath }, std::string { OtherPath } };
    auto fixture = DashboardFixture { world, Wide };
    CHECK(fixture.dashboard.input("w").empty());
    CHECK(fixture.dashboard.input("l").empty());
    CHECK(fixture.dashboard.interaction().work.repoChip == 1);
    CHECK(fixture.dashboard.input("h").empty());
    CHECK(fixture.dashboard.interaction().work.repoChip == 0);
}

TEST_CASE("Work view: on the sort chip, left, right, h and l toggle the sort (Decision 16)",
          "[orchestrator][view][work]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    CHECK(fixture.dashboard.input("w\t\t\t").empty());
    REQUIRE(fixture.dashboard.interaction().work.focus == WorkFocus::SortChip);
    auto const toggle =
        std::vector<UiAction> { ToggleFoldAction { .group = std::string { SortByUpdateKey } } };
    for (auto const* const key: { "l", "\x1b[C", "h", "\x1b[D" })
    {
        CAPTURE(key);
        CHECK(fixture.dashboard.input(key) == toggle);
    }
    CHECK(fixture.dashboard.interaction().work.repoChip == 0);
}

TEST_CASE("Work view: Ctrl+D and Ctrl+U move by half the lines shown; G and gg to the ends",
          "[orchestrator][view][work]")
{
    auto world = workWorld();
    world.prs = { .text = manyPrs(60) };
    world.issues = { .text = "[]" };
    world.answers.clear();
    world.repos = std::string { OneRepository };
    auto fixture = DashboardFixture { world, Medium };
    CHECK(fixture.dashboard.input("w").empty());
    static_cast<void>(fixture.screen());
    auto const& work = fixture.dashboard.interaction().work;
    // A heading and the sixty pull requests; the list shows 18 lines at 80x24.
    constexpr auto Lines = std::size_t { 61 };
    constexpr auto Rows = std::size_t { 18 };
    REQUIRE(work.selected == 1);
    CHECK(fixture.dashboard.input("\x04").empty());
    CHECK(work.selected == 1 + (Rows / 2));
    CHECK(fixture.dashboard.input("\x15").empty());
    CHECK(work.selected == 1);
    CHECK(fixture.dashboard.input("G").empty());
    CHECK(work.selected == Lines - 1);
    CHECK(fixture.dashboard.input("g").empty());
    CHECK(work.selected == Lines - 1);
    CHECK(fixture.dashboard.input("g").empty());
    CHECK(work.selected == 0);
}

TEST_CASE("Work view: the bar names next_value's first key", "[orchestrator][view][work]")
{
    CHECK(workBarText(std::nullopt, 200, NavKeys::defaults(VimKeys::On)).contains("→ repository"));
    auto world = workWorld();
    world.keys = R"({ "next_value": ["l"] })";
    auto fixture = DashboardFixture { world, { .width = 160, .height = 40 } };
    CHECK(fixture.dashboard.input("w").empty());
    CHECK(fixture.screen().contains("l repository"));
}

TEST_CASE("Work view: while the filter is typed, Ctrl+D moves nothing and the arrows still move",
          "[orchestrator][view][work]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    CHECK(fixture.dashboard.input("w/").empty());
    auto const& work = fixture.dashboard.interaction().work;
    REQUIRE(work.filtering == Filtering::Typing);
    auto const selected = work.selected;
    CHECK(fixture.dashboard.input("\x04").empty());
    CHECK(work.selected == selected);
    CHECK(work.filtering == Filtering::Typing);
    CHECK(fixture.dashboard.input("\x1b[B").empty());
    CHECK(work.selected == selected + 1);
    CHECK(work.filtering == Filtering::Typing);
}

TEST_CASE("Work view: the filter's esc keeps it and a second esc clears it; / types it again",
          "[orchestrator][view][work][workboard]")
{
    auto fixture = DashboardFixture { workWorld(), Wide };
    CHECK(fixture.dashboard.input("w/jkq").empty());
    auto const& work = fixture.dashboard.interaction().work;
    CHECK(work.filter == "jkq");
    CHECK(work.filtering == Filtering::Typing);
    CHECK(fixture.dashboard.input("\x1b").empty());
    static_cast<void>(fixture.escapeTimesOut());
    CHECK(work.filter == "jkq");
    CHECK(work.filtering == Filtering::Off);
    CHECK(work.focus == WorkFocus::List);
    CHECK(fixture.screen().contains("/ jkq"));
    // `/` types it again with its text kept.
    CHECK(fixture.dashboard.input("/x").empty());
    CHECK(work.filter == "jkqx");
    CHECK(work.filtering == Filtering::Typing);
    CHECK(fixture.dashboard.input("\x1b").empty());
    static_cast<void>(fixture.escapeTimesOut());
    CHECK(fixture.dashboard.input("\x1b").empty());
    static_cast<void>(fixture.escapeTimesOut());
    CHECK(work.filter.empty());
    CHECK(work.filtering == Filtering::Off);
}

TEST_CASE("Work view: the automation chip shows the switch, and only with rules",
          "[orchestrator][view][work][automation]")
{
    auto without = DashboardFixture { workWorld(), Wide };
    CHECK(without.dashboard.input("w").empty());
    CHECK_FALSE(without.screen().contains("auto "));

    auto world = workWorld();
    world.automation = R"({ "rules": [ { "when": "pipeline_failed", "task": "fix_ci" } ] })";
    auto fixture = DashboardFixture { world, Wide };
    CHECK(fixture.dashboard.input("w").empty());
    CHECK(fixture.screen().contains("auto ⏸ paused"));

    // tab moves the keys along the filter bar's chips to the automation chip; ⏎ switches it.
    CHECK(fixture.dashboard.input("\t\t\t\t").empty());
    CHECK(fixture.dashboard.interaction().work.focus == WorkFocus::AutomationChip);
    auto const actions = fixture.dashboard.input("\r");
    REQUIRE(actions.size() == 1);
    CHECK(actions.front() == UiAction { SetAutomationAction { .to = AutomationSwitch::On } });
}

TEST_CASE("Work view: an item waiting for a free slot shows it is queued",
          "[orchestrator][view][work][automation]")
{
    auto world = workWorld();
    world.automation = R"({ "enabled": true, "max_agents": 1,
                            "rules": [ { "when": "pipeline_failed", "task": "fix_ci" },
                                       { "when": "conflicts", "task": "fix_ci" } ] })";
    auto fixture = DashboardFixture { world, Wide };
    CHECK(fixture.dashboard.input("w").empty());
    CHECK(fixture.screen().contains("⚙ queued"));
}
