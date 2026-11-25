// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/Responsive.hpp>
#include <core/tui/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Stats.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/Band.hpp>
#include <orchestrator/view/BoardMarkdown.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/Fleet.hpp>
#include <orchestrator/view/Focus.hpp>
#include <orchestrator/view/HitMap.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/RowMenu.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/Theme.hpp>
#include <orchestrator/view/ViewFixtures.hpp>
#include <orchestrator/view/WorkBoard.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;
using core::tui::Size;

// {{{ helpers

namespace
{

constexpr auto RunningId = "pr-2375-20261113-150847"sv;
constexpr auto DoneId = "pr-2379-20261113-150912"sv;
constexpr auto NewId = "pr-2366-20261113-140211"sv;
constexpr auto KeptId = "issue-2356-20261113-151001"sv;

constexpr auto Wide = Size { .width = 120, .height = 40 };

/// Which way the wheel turns.
enum class MouseWheel : uint8_t
{
    Up = 0,
    Down,
};
constexpr auto Medium = Size { .width = 80, .height = 24 };
constexpr auto Narrow = Size { .width = 50, .height = 20 };

using Fixture = DashboardFixture;

[[nodiscard]] Run finishedRun(std::string_view id, ItemRef item, Fate fate, Seen seen, std::string_view task)
{
    auto entries = fate == Fate::Kept
                       ? std::vector<std::string> { " M src/vtbackend/Grid.cpp", "?? notes.txt" }
                       : std::vector<std::string> {};
    return storedRun(Finished { .ended = fate == Fate::Kept ? Ended::EndedByYou : Ended::Completed,
                                .fate = fate,
                                .entries = std::move(entries),
                                .seen = seen,
                                .endedWallMs = DispatchWallMs },
                     id,
                     std::move(item),
                     task);
}

/// A running agent that never reported a status: what a folder-trust prompt looks like from outside.
[[nodiscard]] Run unreportedRun()
{
    auto run = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    run.lastStatus = std::nullopt;
    return run;
}

/// A run that finished as completed, its statistics read and its history kept.
[[nodiscard]] Run finishedWithStats()
{
    auto run = finishedRun(DoneId, ChangeRequestRef { 2379 }, Fate::Removed, Seen::Opened, "fix_ci");
    run.lastStatus = AgentStatus::Done;
    run.history = {
        StateChange { .wallMs = DispatchWallMs, .state = "preparing" },
        StateChange { .wallMs = DispatchWallMs + 20'000, .state = "running" },
        StateChange { .wallMs = DispatchWallMs + 300'000, .state = "blocked" },
        StateChange { .wallMs = DispatchWallMs + 395'000, .state = "running" },
        StateChange { .wallMs = DispatchWallMs + 1'800'000, .state = "done" },
        StateChange { .wallMs = DispatchWallMs + 1'830'000, .state = "ending" },
        StateChange { .wallMs = DispatchWallMs + 1'840'000, .state = "finished" },
    };
    run.stats = parseStats(readFixture(FixtureRoot::Contract, "agent-stats/format-1/basic.json")).value();
    run.stats->tokens = StatsTokens {
        .input = 152, .output = 42'348, .thinking = 16'692, .cacheWrite = 192'679, .cacheRead = 12'895'510
    };
    run.stats->subagents = StatsSubagents { .count = 2, .apiCalls = 9, .tokens = { .output = 3'100 } };
    return run;
}

/// A run whose tab the host refused to open: agents cannot start in a worktree from this sandbox.
[[nodiscard]] Run paneRefusedRun()
{
    auto run = storedRun(Finished { .ended = Ended::PreparationFailed,
                                    .fate = Fate::Removed,
                                    .entries = {},
                                    .seen = Seen::New,
                                    .endedWallMs = DispatchWallMs },
                         NewId,
                         ChangeRequestRef { 2366 },
                         "fix_ci");
    run.failure = RunFailure { .step = PrepareStep::OpenPane, .why = "not-supported-yet" };
    return run;
}

/// What a kept run records when its checks stopped at `common-dir`.
constexpr auto CommonDirReason =
    "git uses another directory than the main checkout's .git; nothing more was run there"sv;

/// Two worktrees no run accounts for, the last one's path holding a space and a `'`.
[[nodiscard]] World orphansWorld()
{
    return World { .prs = { "github/prs-empty.json" },
                   .worktrees = {
                       WorktreeEntry { .path = "/home/u/worktrees/contour/pr-2-20260101-000000",
                                       .branch = std::nullopt },
                       WorktreeEntry { .path = "/home/u/worktrees/contour/it's a test",
                                       .branch = "agent/pr-1-20260101-000000" },
                   } };
}

/// @return The page an `agent-transcript` command asks for.
[[nodiscard]] std::string pageOf(RunCommand const& command)
{
    auto const page = std::ranges::find(command.arguments, std::string_view { "page" }, &Argument::name);
    return page != command.arguments.end() ? page->value : std::string {};
}

/// What a status board an agent wrote says, with its task boxes.
constexpr auto WorkingBoard = R"(# Fixing CI
- [x] reproduced on master
- [x] wrote the regression test
- [ ] fix the reflow
- [ ] watch CI
)"sv;

/// A world with a run in every group: one that asks, one waiting with its reason, one at work, one done, and
/// three finished (removed, kept and failed).
[[nodiscard]] World fleetWorld()
{
    constexpr auto AskingId = "pr-2346-20261113-150500"sv;
    constexpr auto WaitingId = "issue-2253-20261113-150600"sv;
    auto asking = storedRun(Blocked {}, AskingId, ChangeRequestRef { 2346 });
    asking.lastStatus = AgentStatus::Asking;
    asking.history = { StateChange { .wallMs = DispatchWallMs - 120'000, .state = "blocked" } };
    auto waiting = storedRun(Running {}, WaitingId, IssueRef { 2253 }, "issue");
    waiting.lastStatus = AgentStatus::Idle;
    waiting.history = { StateChange { .wallMs = DispatchWallMs - 60'000, .state = "running" } };
    auto working = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    working.history = { StateChange { .wallMs = DispatchWallMs - 600'000, .state = "running" } };
    auto done = storedRun(Done {}, DoneId, ChangeRequestRef { 2379 });
    auto kept = finishedRun(KeptId, IssueRef { 2356 }, Fate::Kept, Seen::Opened, "issue");
    std::get<Finished>(kept.state).endedWallMs = DispatchWallMs - 60'000;
    auto failed = storedRun(Finished { .ended = Ended::PreparationFailed,
                                       .fate = Fate::Removed,
                                       .entries = {},
                                       .seen = Seen::New,
                                       .endedWallMs = DispatchWallMs - 120'000 },
                            "pr-2114-20261113-120000",
                            ChangeRequestRef { 2114 });
    failed.failure = RunFailure { .step = PrepareStep::OpenPane, .why = "not-supported-yet" };
    return World {
        .stored = { working,
                    asking,
                    waiting,
                    done,
                    finishedRun(NewId, ChangeRequestRef { 2366 }, Fate::Removed, Seen::New, "fix_ci"),
                    kept,
                    failed },
        .paneMessages = { { std::string { WaitingId }, "Waiting for Build pace" },
                          { std::string { AskingId }, "Bash: ctest -R reflow --output-on-failure" } },
    };
}

/// The answers the guest gives the dashboard's reads in the golden scenes.
[[nodiscard]] Answers sceneAnswers()
{
    return Answers { .transcript = printed(
                         readFixture(FixtureRoot::Contract, "agent-transcript/format-1/basic-page-1.json")),
                     .board = std::string { WorkingBoard },
                     .boardModifiedWallMs = DispatchWallMs - 180'000 };
}

/// The worlds the golden screens are drawn from.
struct Scene
{
    std::string_view name = {};
    World world = {};
    std::string_view keys = {};                         ///< Typed before the screen is drawn.
    std::vector<Size> sizes = { Wide, Medium, Narrow }; ///< The sizes it is drawn at.
    std::string_view select = {};                       ///< A run selected before the keys; empty: none.
    std::string_view then = {}; ///< Typed after a frame of the keys, such as a click on what they showed.
};

[[nodiscard]] std::vector<Scene> scenes()
{
    auto const keptReasons =
        World { .prs = { "github/prs-empty.json" },
                .stored = { storedRun(Finished { .ended = Ended::EndedByYou,
                                                 .fate = Fate::Kept,
                                                 .entries = { std::string { CommonDirReason } },
                                                 .seen = Seen::Opened,
                                                 .endedWallMs = DispatchWallMs },
                                      KeptId,
                                      IssueRef { 2356 },
                                      "issue") } };
    auto const paneRefused = World { .prs = { "github/prs-empty.json" }, .stored = { paneRefusedRun() } };
    auto workEmptyGroups = workWorld();
    workEmptyGroups.work = R"({ "fold_empty": false })";
    auto const withStats = World { .prs = { "github/prs-empty.json" }, .stored = { finishedWithStats() } };
    auto const twoRuns = World { .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                                             storedRun(Done {}, DoneId, ChangeRequestRef { 2379 }) } };
    return {
        Scene { .name = "empty", .world = World { .configJson = "{}" } },
        // Configured, nothing run yet: the primer.
        Scene { .name = "fleet-empty", .world = World {} },
        Scene { .name = "problem", .world = World { .prs = { "" } }, .sizes = { Wide } },
        // Every group; the working run selected, on its Board.
        Scene { .name = "fleet", .world = fleetWorld(), .select = RunningId },
        // The palette: the working run's actions, the global commands, the rest dimmed with the reason.
        Scene { .name = "palette", .world = fleetWorld(), .keys = "\x0b", .select = RunningId },
        Scene { .name = "palette-query", .world = fleetWorld(), .keys = "\x0bre", .sizes = { Wide }, .select = KeptId },
        Scene { .name = "palette-empty", .world = World {}, .keys = "?", .sizes = { Medium } },
        // The last 24 hours, opened from the palette.
        Scene { .name = "timeline", .world = fleetWorld(), .keys = "\x0blast 24\r", .sizes = { Wide, Narrow } },
        // Each tab of the Focus, chosen with its letter.
        Scene { .name = "focus-board", .world = fleetWorld(), .keys = "b", .sizes = { Wide }, .select = RunningId },
        Scene { .name = "focus-transcript",
                .world = fleetWorld(),
                .keys = "t",
                .sizes = { Wide, Medium, Narrow },
                .select = RunningId },
        Scene { .name = "focus-details", .world = fleetWorld(), .keys = "d", .sizes = { Wide }, .select = RunningId },
        // The run that asks opens on its transcript, its request in the asking box; in the Narrow band its row's
        // second line holds the request, and tab shows the box.
        Scene { .name = "asking", .world = fleetWorld() },
        Scene { .name = "asking-focus", .world = fleetWorld(), .keys = "\t", .sizes = { Medium, Narrow } },
        // The message box has the keys, and a draft.
        Scene { .name = "message-box",
                .world = fleetWorld(),
                .keys = "\rrebase on master first",
                .sizes = { Wide, Medium, Narrow },
                .select = RunningId },
        Scene { .name = "done-run", .world = twoRuns, .select = DoneId },
        Scene { .name = "confirm-end", .world = twoRuns, .keys = "e", .select = RunningId },
        Scene { .name = "finished", .world = fleetWorld(), .sizes = { Wide, Medium }, .select = NewId },
        Scene { .name = "outside",
                .world = World { .prs = { "github/prs-empty.json" },
                                 .stored = {
                                     outsideConfiguration(storedRun(Running {}, RunningId, ChangeRequestRef { 2375 })),
                                     outsideConfiguration(
                                         finishedRun(KeptId, IssueRef { 2356 }, Fate::Kept, Seen::Opened, "issue")),
                                 } },
                .sizes = { Wide, Narrow } },
        Scene { .name = "trust-hint",
                .world = World { .prs = { "github/prs-empty.json" }, .stored = { unreportedRun() } },
                .sizes = { Medium } },
        Scene { .name = "kept-reasons", .world = keptReasons, .sizes = { Wide } },
        Scene { .name = "kept-reasons", .world = keptReasons, .keys = "\t", .sizes = { Narrow } },
        Scene { .name = "finished-stats", .world = withStats, .sizes = { Wide } },
        Scene { .name = "finished-stats", .world = withStats, .keys = "\t", .sizes = { Narrow } },
        Scene { .name = "pane-refused", .world = paneRefused, .sizes = { Wide } },
        Scene { .name = "orphans", .world = orphansWorld(), .keys = "\x1b[Hj", .sizes = { Wide, Medium } },
        // Tab gives an orphan's details the whole tab in the Narrow band.
        Scene { .name = "orphans", .world = orphansWorld(), .keys = "\x1b[Hj\t", .sizes = { Narrow } },
        // The Work view: grouped by importance, Everything else folded.
        Scene { .name = "work", .world = workWorld(), .keys = "w", .sizes = { Narrow, Medium, Wide, { .width = 160, .height = 40 } } },
        // Needs you folded from its first item, its heading selected.
        Scene { .name = "work-folded", .world = workWorld(), .keys = "wz", .sizes = { Medium, Wide } },
        // One flat list, newest first.
        Scene { .name = "work-flat", .world = workWorld(), .keys = "ws", .sizes = { Medium, Wide } },
        // The filter being typed.
        Scene { .name = "work-filter", .world = workWorld(), .keys = "w/prj", .sizes = { Medium, Wide } },
        // `work.fold_empty: false`: a group the filter leaves empty keeps its heading.
        Scene { .name = "work-empty-groups", .world = workEmptyGroups, .keys = "w/2375", .sizes = { Medium } },
        // The start dialog on Needs you's first item: its suggested task with the reason, the worker chips.
        Scene { .name = "start-dialog", .world = workWorld(), .keys = "w\r", .sizes = { Narrow, Medium, Wide } },
        // The `n` box, a task typed: the defaults it starts on, the branch and worktree it makes.
        Scene { .name = "task-box", .world = World {}, .keys = "ninvestigate the flaky test", .sizes = { Narrow, Medium, Wide } },
        // The row menu, opened with the right button on Needs you's first item: #2375's.
        Scene { .name = "row-menu",
                .world = workWorld(),
                .keys = "w",
                .sizes = { Medium, Wide },
                .then = "[<2;31;5M[<2;31;5m" },
    };
}

} // namespace

// }}}
// {{{ bands and breakpoints

static_assert(bandOf(59) == Band::Narrow);
static_assert(bandOf(60) == Band::Medium);
static_assert(bandOf(99) == Band::Medium);
static_assert(bandOf(100) == Band::Wide);

TEST_CASE("the breakpoints are ordered as choose() requires", "[orchestrator][view]")
{
    auto breakpoints = std::vector<core::tui::Breakpoint> {};
    for (auto const& row: BandBreakpoints)
        breakpoints.push_back(
            core::tui::Breakpoint { .minColumns = row.minColumns, .minRows = 0, .make = {} });
    struct Case
    {
        Size size = {};
        std::size_t index = 0;
    };
    auto const cases = std::array {
        Case { .size = { .width = 59, .height = 19 }, .index = 0 },
        Case { .size = { .width = 60, .height = 19 }, .index = 1 },
        Case { .size = { .width = 99, .height = 40 }, .index = 1 },
        Case { .size = { .width = 100, .height = 12 }, .index = 2 },
    };
    for (auto const& [size, index]: cases)
    {
        CAPTURE(size.width, size.height);
        CHECK(core::tui::Responsive::choose(breakpoints, size) == std::optional { index });
        CHECK(BandBreakpoints[index].band == bandOf(size.width));
    }
}

// }}}
// {{{ golden screens

TEST_CASE("golden screens", "[orchestrator][view]")
{
    for (auto const& scene: scenes())
        for (auto const size: scene.sizes)
        {
            auto const name = std::format("{}-{}x{}", scene.name, size.width, size.height);
            SECTION(name)
            {
                auto fixture = Fixture { scene.world, size };
                static_cast<void>(fixture.dashboard.frame());
                if (!scene.select.empty())
                    CHECK(fixture.dashboard.select(scene.select).empty());
                // The guest's turn: the Focus run's board and transcript are read, and drawn.
                fixture.settle(sceneAnswers());
                static_cast<void>(fixture.dashboard.frame());
                // What the keys ask, such as a fold, is carried out as the guest does.
                static_cast<void>(fixture.carryOut(fixture.dashboard.input(scene.keys), sceneAnswers()));
                fixture.settle(sceneAnswers());
                // A click lands on what the last frame drew.
                if (!scene.then.empty())
                {
                    static_cast<void>(fixture.dashboard.frame());
                    static_cast<void>(fixture.carryOut(fixture.dashboard.input(scene.then), sceneAnswers()));
                    fixture.settle(sceneAnswers());
                }
                checkGolden(name, fixture.screen());
            }
        }
}

TEST_CASE("a resize repaints at the new band", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld(), Wide };
    CHECK(fixture.screen().contains("Board"));
    CHECK(fixture.dashboard.breakpoint() == std::optional<std::size_t> { 2 });
    fixture.dashboard.resize(Narrow);
    auto const bytes = fixture.dashboard.frame();
    CHECK(!bytes.empty());
    CHECK(fixture.dashboard.breakpoint() == std::optional<std::size_t> { 0 });
    // The Narrow band shows the Fleet alone while it has the keys.
    auto const narrow = core::tui::test::canvasToString(fixture.dashboard.rendered());
    CHECK(!narrow.contains("Transcript"));
    CHECK(fixture.dashboard.rendered().cols() == Narrow.width);
}

TEST_CASE("the dashboard shows no cursor, and rests it at the top-left", "[orchestrator][view]")
{
    constexpr auto ShowCursor = "\033[?25h"sv;
    for (auto const size: { Wide, Medium, Narrow })
    {
        auto fixture = Fixture { fleetWorld(), size };
        // Each frame draws into the other of the screen's two buffers, so several are looked at, among them
        // the frames that open the filter, the help and the message box.
        for (auto const keys: { ""sv, "j"sv, "/"sv, "\033"sv, "?"sv, "\033"sv, "\r"sv, "x"sv })
        {
            static_cast<void>(fixture.dashboard.input(keys));
            fixture.clock.advance(std::chrono::milliseconds { 60 });
            auto const bytes = fixture.dashboard.frame();
            CHECK(!bytes.contains(ShowCursor));
            CHECK(!fixture.dashboard.rendered().cursorVisible());
            // At rest on the top row, so a shorter tab cuts its bottom rows instead of pushing the top ones
            // up.
            CHECK((bytes.empty() || bytes.ends_with(RestingCursor)));
        }
    }
}

// }}}
// {{{ the Fleet

TEST_CASE("the Fleet lists the runs by what they need, the longest waiting first",
          "[orchestrator][view][fleet]")
{
    auto const engine = bootEngine(fleetWorld());
    auto const rows = buildRows(engine, StartMonoMs);
    auto order = std::vector<std::string> {};
    for (auto const& row: rows)
        order.push_back(row.kind == RowKind::Heading ? row.text : row.name.empty() ? row.text : row.name);
    CHECK(order
          == std::vector<std::string> { "NEEDS YOU",
                                        "pr-2346",
                                        "issue-2253",
                                        "WORKING",
                                        "pr-2375",
                                        "TO REVIEW",
                                        "pr-2379",
                                        "FINISHED",
                                        "pr-2366",
                                        "issue-2356",
                                        "pr-2114",
                                        "new task" });
    // An idle agent's reason is its second line; a blocked one asks.
    auto const waiting = std::ranges::find(rows, "issue-2253", &Row::name);
    REQUIRE(waiting != rows.end());
    CHECK(waiting->context == "Waiting for Build pace");
    CHECK(waiting->status.starts_with("waiting"));
    auto const asking = std::ranges::find(rows, "pr-2346", &Row::name);
    REQUIRE(asking != rows.end());
    CHECK(asking->status.starts_with("asking"));
}

TEST_CASE("a run that never reported is starting, in Working", "[orchestrator][view][fleet]")
{
    auto const engine =
        bootEngine(World { .prs = { "github/prs-empty.json" }, .stored = { unreportedRun() } });
    auto const rows = buildRows(engine, StartMonoMs);
    auto const row = std::ranges::find(rows, RunningId, &Row::run);
    REQUIRE(row != rows.end());
    CHECK(row->attention == FleetGroup::Working);
    CHECK(row->state == "starting");
    CHECK(row->context == "waiting for the agent's first report");
}

TEST_CASE("Finished is folded to its newest five; enter on its heading shows all",
          "[orchestrator][view][fleet]")
{
    auto world = World { .prs = { "github/prs-empty.json" } };
    for (auto const index: std::views::iota(0, 7))
    {
        auto run = finishedRun(std::format("pr-{}-20261113-1500{:02}", 3000 + index, index),
                               ChangeRequestRef { static_cast<uint64_t>(3000 + index) },
                               Fate::Removed,
                               Seen::Opened,
                               "fix_ci");
        std::get<Finished>(run.state).endedWallMs = DispatchWallMs + (static_cast<uint64_t>(index) * 1000);
        world.stored.push_back(std::move(run));
    }
    auto fixture = Fixture { world };
    auto const folded = fixture.screen();
    CHECK(folded.contains("FINISHED 5 of 7 · enter shows all"));
    CHECK(folded.contains("pr-3006"));
    CHECK_FALSE(folded.contains("pr-3001 "));
    // The heading is selected first: it hides some runs.
    CHECK(fixture.dashboard.input("\r")
          == std::vector<UiAction> { ToggleFoldAction { .group = "fleet/finished" } });
    static_cast<void>(fixture.engine.step(ToggleFold { .group = "fleet/finished", .monoMs = StartMonoMs }));
    fixture.dashboard.engineChanged();
    auto const open = fixture.screen();
    CHECK(open.contains("pr-3001 "));
    CHECK(open.contains("pr-3000 "));
    // A filter looks at every run, folded or not.
    static_cast<void>(fixture.engine.step(ToggleFold { .group = "fleet/finished", .monoMs = StartMonoMs }));
    fixture.dashboard.engineChanged();
    CHECK(fixture.dashboard.input("/3000\r").empty());
    CHECK(fixture.screen().contains("pr-3000 "));
}

TEST_CASE("the selection stays on its run when the run changes groups", "[orchestrator][view][fleet]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.select(RunningId).empty());
    static_cast<void>(fixture.screen());
    // The working run starts to ask: it moves to Needs you, above, and keeps the selection.
    static_cast<void>(
        fixture.engine.step(PanesListed { .panes = { PaneFact { .pane = std::format("p-{}", RunningId),
                                                                .tag = std::string { RunningId },
                                                                .status = AgentStatus::Asking,
                                                                .message = {},
                                                                .program = ProgramLiveness::Alive } },
                                          .requestedMonoMs = StartMonoMs + 1'000,
                                          .listing = PaneListing::Tagged }));
    fixture.dashboard.engineChanged();
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.selectedRun() == RunningId);

    // And a run that enters Needs you never takes the selection from another.
    CHECK(fixture.dashboard.select(DoneId).empty());
    static_cast<void>(
        fixture.engine.step(PanesListed { .panes = { PaneFact { .pane = std::format("p-{}", DoneId),
                                                                .tag = std::string { DoneId },
                                                                .status = AgentStatus::Error,
                                                                .message = {},
                                                                .program = ProgramLiveness::Alive } },
                                          .requestedMonoMs = StartMonoMs + 2'000,
                                          .listing = PaneListing::Tagged }));
    fixture.dashboard.engineChanged();
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.selectedRun() == DoneId);
}

TEST_CASE("a board's task boxes are its meter and its first open box its row's second line",
          "[orchestrator][view][fleet]")
{
    auto const lines =
        renderBoardMarkdown(std::vector<std::string> { "- [x] a", "  - [ ] b", "- [ ] c", "- [X] d" });
    auto const progress = boardProgressOf(lines);
    REQUIRE(progress.has_value());
    CHECK(*progress == BoardProgress { .done = 2, .total = 4, .current = "b" });
    CHECK(meterOf(*progress, 6) == "▰▰▰▱▱▱");
    CHECK(meterOf(BoardProgress { .done = 1, .total = 100, .current = {} }, 6) == "▰▱▱▱▱▱");
    CHECK(meterOf(BoardProgress { .done = 99, .total = 100, .current = {} }, 6) == "▰▰▰▰▰▱");
    CHECK(boardProgressOf(renderBoardMarkdown(std::vector<std::string> { "# no boxes" })) == std::nullopt);
}

TEST_CASE("the furniture gives way in its order", "[orchestrator][view][fleet]")
{
    // Plenty of rows: nothing gives way.
    CHECK(furnitureGivingWay(
              FocusRows { .height = 38, .topBarRows = 1, .bottomRows = 3, .tab = FocusTab::Board })
          == Furniture::None);
    // 80x24, the Board tab below the Fleet: the header's line, latest, the top bar and the strip, in turn.
    CHECK(furnitureGivingWay(
              FocusRows { .height = 13, .topBarRows = 1, .bottomRows = 3, .tab = FocusTab::Board })
          == (Furniture::HeaderDetail | Furniture::Latest | Furniture::TopBar));
    CHECK(furnitureGivingWay(
              FocusRows { .height = 12, .topBarRows = 1, .bottomRows = 3, .tab = FocusTab::Board })
          == (Furniture::HeaderDetail | Furniture::Latest | Furniture::TopBar | Furniture::TabStrip));
    // The Transcript tab has no latest to give.
    CHECK(furnitureGivingWay(
              FocusRows { .height = 12, .topBarRows = 1, .bottomRows = 3, .tab = FocusTab::Transcript })
          == (Furniture::HeaderDetail | Furniture::TopBar | Furniture::TabStrip));
    // The Details tab keeps latest: only the Board tab's gives way.
    CHECK(holds(furnitureGivingWay(
                    FocusRows { .height = 13, .topBarRows = 1, .bottomRows = 3, .tab = FocusTab::Details }),
                Furniture::TabStrip));
    CHECK_FALSE(holds(furnitureGivingWay(FocusRows {
                          .height = 13, .topBarRows = 1, .bottomRows = 3, .tab = FocusTab::Details }),
                      Furniture::Latest));
}

// }}}
// {{{ the Focus

TEST_CASE("the transcript of a live run is read on selection and again every five seconds",
          "[orchestrator][view][transcript]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    // A page longer than the tab, so that it can be scrolled away from its end.
    auto entries = std::string {};
    for (auto const index: std::views::iota(0, 30))
        entries +=
            std::format(R"({}{{"kind":"assistant","time":"2026-11-19T18:00:00.000Z","text":"entry {}"}})",
                        index == 0 ? "" : ",",
                        index);
    auto const answers =
        Answers { .transcript = printed(std::format(
                      R"({{"format":1,"session":"00000000-0000-4000-8000-000000000001",)"
                      R"("writtenBy":"2.1.328","page":1,"pages":1,"notUnderstood":0,"entries":[{}]}})",
                      entries)) };
    CHECK(fixture.settle(answers).size() == 1);
    // Nothing more until five seconds have passed.
    fixture.clock.advance(std::chrono::seconds { 4 });
    CHECK(fixture.settle(answers).empty());
    auto const due = fixture.dashboard.nextRepaintDue();
    REQUIRE(due.has_value());
    CHECK(*due <= fixture.clock.now() + std::chrono::seconds { 1 });
    fixture.clock.advance(std::chrono::seconds { 1 });
    auto const again = fixture.settle(answers);
    REQUIRE(again.size() == 1);
    CHECK(pageOf(again.front()) == "1");

    // Scrolled up, it stops following: no more reads, and `↓ live` shows.
    CHECK(fixture.dashboard.input("t\t").empty());
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.input("\x1b[5~").empty());
    CHECK(fixture.dashboard.interaction().follow == Follow::Away);
    fixture.clock.advance(std::chrono::seconds { 10 });
    CHECK(fixture.settle(answers).empty());
    CHECK(fixture.screen().contains("↓ live"));
    // End returns to it, and it is read again.
    CHECK(fixture.dashboard.input("\x1b[F").empty());
    CHECK(fixture.dashboard.interaction().follow == Follow::Live);
    CHECK(fixture.settle(answers).size() == 1);
    CHECK_FALSE(fixture.screen().contains("↓ live"));
}

TEST_CASE("a finished run's transcript is read once", "[orchestrator][view][transcript]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(NewId).empty());
    CHECK(fixture.settle(sceneAnswers()).size() == 1);
    fixture.clock.advance(std::chrono::seconds { 30 });
    CHECK(fixture.settle(sceneAnswers()).empty());
}

TEST_CASE("a transcript that grew a page is read at its last page", "[orchestrator][view][transcript]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    auto const page = [](uint32_t number, uint32_t pages) {
        return printed(
            std::format(R"({{"format":1,"session":"00000000-0000-4000-8000-000000000001",)"
                        R"("writtenBy":"2.1.328","page":{},"pages":{},"notUnderstood":0,"entries":[]}})",
                        number,
                        pages));
    };
    REQUIRE(fixture.settle(Answers { .transcript = page(1, 3) }).size() == 1);
    auto const last = fixture.settle(Answers { .transcript = page(3, 3) });
    REQUIRE(last.size() == 1);
    CHECK(pageOf(last.front()) == "3");
}

TEST_CASE("latest shows a message sent until the transcript does", "[orchestrator][view][message]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    fixture.settle(sceneAnswers());
    auto const sent = fixture.dashboard.input("\rrebase first\r");
    CHECK(sent
          == std::vector<UiAction> {
              SendMessageAction { .run = std::string { RunningId }, .text = "rebase first" } });
    CHECK(fixture.dashboard.interaction().place == Place::Fleet);
    CHECK(fixture.dashboard.selectedRun() == RunningId);
    auto const screen = fixture.screen();
    CHECK(screen.contains("you · sent 15:08 · rebase first"));
    CHECK(screen.contains("sent to pr-2375"));
}

TEST_CASE("a refused message comes back to the box, with the keys", "[orchestrator][view][message]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK_FALSE(fixture.dashboard.input("\rrebase\r").empty());
    fixture.dashboard.messageRefused("the host will not type this text");
    CHECK(fixture.dashboard.interaction().place == Place::Focus);
    CHECK(fixture.dashboard.interaction().drafts.at(std::string { RunningId }) == "rebase");
    CHECK(fixture.screen().contains("message not sent: the host will not type this text"));
}

TEST_CASE("drafts are kept per run", "[orchestrator][view][message]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK(fixture.dashboard.input("\rfirst").empty());
    fixture.escapeTimesOut();
    CHECK(fixture.dashboard.select(DoneId).empty());
    CHECK(fixture.dashboard.input("\rsecond").empty());
    fixture.escapeTimesOut();
    auto const& drafts = fixture.dashboard.interaction().drafts;
    CHECK(drafts.at(std::string { RunningId }) == "first");
    CHECK(drafts.at(std::string { DoneId }) == "second");
}

// {{{ quick replies

namespace
{
constexpr auto AskingRun = "pr-2346-20261113-150500"sv;

/// The fleet world drawn, the asking run selected and its reads answered.
[[nodiscard]] std::unique_ptr<Fixture> askingFixture(Size size = Wide)
{
    auto fixture = std::make_unique<Fixture>(fleetWorld(), size);
    static_cast<void>(fixture->dashboard.frame());
    CHECK(fixture->dashboard.select(AskingRun).empty());
    fixture->settle(sceneAnswers());
    return fixture;
}
} // namespace

TEST_CASE("a digit answers the run that asks: its chip fills and the box dims", "[orchestrator][view][reply]")
{
    auto fixture = askingFixture();
    CHECK(fixture->dashboard.input("1")
          == std::vector<UiAction> { QuickReplyAction { .run = std::string { AskingRun },
                                                        .key = '1',
                                                        .label = "yes",
                                                        .press = PaneKey::Digit1,
                                                        .then = AfterReply::Wait } });
    REQUIRE(fixture->dashboard.interaction().answered.has_value());
    CHECK(fixture->dashboard.interaction().answered->key == '1');
    // The keys stay where they were: the agent was answered, nothing waits to be written.
    CHECK(fixture->dashboard.interaction().place == Place::Fleet);
    checkGolden("asking-answered-120x40", fixture->screen());
    // A second digit while the first is in flight would reach the agent's prompt, not its menu.
    CHECK(fixture->dashboard.input("2").empty());
    CHECK(fixture->screen().contains("pr-2346 is answered · r opens its tab"));
}

TEST_CASE("3 rejects with Escape and opens the message box for the reason", "[orchestrator][view][reply]")
{
    auto fixture = askingFixture();
    CHECK(fixture->dashboard.input("3")
          == std::vector<UiAction> { QuickReplyAction { .run = std::string { AskingRun },
                                                        .key = '3',
                                                        .label = "no, and say why",
                                                        .press = PaneKey::Escape,
                                                        .then = AfterReply::Message } });
    CHECK(fixture->dashboard.interaction().place == Place::Focus);
    CHECK(fixture->screen().contains("answered no, and say why"));
}

TEST_CASE("a digit answers only while the selected run asks", "[orchestrator][view][reply]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    fixture.settle(sceneAnswers());
    CHECK(fixture.dashboard.input("1").empty());
    CHECK(fixture.screen().contains("select the request first"));
    // In the message box a digit is text.
    CHECK(fixture.dashboard.input("\r12").empty());
    CHECK(fixture.dashboard.interaction().drafts.at(std::string { RunningId }) == "12");

    // A digit no reply is on says so.
    auto asking = askingFixture();
    CHECK(asking->dashboard.input("7").empty());
    CHECK(asking->screen().contains("no quick reply on 7"));
}

TEST_CASE("an agent still asking 10 s after its reply is pointed at its tab", "[orchestrator][view][reply]")
{
    auto fixture = askingFixture();
    CHECK_FALSE(fixture->dashboard.input("1").empty());
    static_cast<void>(fixture->dashboard.frame());
    auto const due = fixture->dashboard.nextRepaintDue();
    REQUIRE(due.has_value());
    CHECK(*due <= fixture->clock.now() + std::chrono::milliseconds { StillAsksAfterMs });
    fixture->clock.advance(std::chrono::milliseconds { StillAsksAfterMs });
    CHECK(fixture->screen().contains("pr-2346 still asks · r opens its tab"));
    // Its chips are live again.
    CHECK_FALSE(fixture->dashboard.interaction().answered.has_value());
}

TEST_CASE("a reply the guest could not press empties its chip and says why", "[orchestrator][view][reply]")
{
    auto fixture = askingFixture();
    CHECK_FALSE(fixture->dashboard.input("3").empty());
    fixture->dashboard.replyOutdated(AskingRun);
    CHECK_FALSE(fixture->dashboard.interaction().answered.has_value());
    CHECK(fixture->dashboard.interaction().place == Place::Fleet);
    CHECK(fixture->screen().contains("pr-2346 is no longer asking"));

    CHECK_FALSE(fixture->dashboard.input("1").empty());
    fixture->dashboard.replyRefused("the extension may not press keys in its tabs");
    CHECK(fixture->screen().contains("reply not sent: the extension may not press keys in its tabs"));
}

TEST_CASE("the palette lists an asking run's replies by their labels", "[orchestrator][view][reply]")
{
    auto fixture = askingFixture();
    CHECK(fixture->dashboard.input("\x0b").empty());
    auto const screen = fixture->screen();
    CHECK(screen.contains("yes, don't ask again"));
    CHECK(screen.contains("no, and say why"));
}

// }}}

TEST_CASE("o asks to open the link, and the bar says how it went", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.input("o")
          == std::vector<UiAction> { OpenUrlAction {
              .url = "https://github.com/contour-terminal/contour/pull/2375", .label = "#2375" } });
    // No OSC 52: a served tab drops it.
    CHECK_FALSE(fixture.dashboard.frame().contains("\x1b]52;"));
    fixture.dashboard.urlOpened("#2375");
    static_cast<void>(fixture.dashboard.frame());
    CHECK(core::tui::test::canvasToString(fixture.dashboard.rendered())
              .contains("opening #2375 on your machine"));
    fixture.dashboard.urlRefused("this terminal cannot open links");
    static_cast<void>(fixture.dashboard.frame());
    CHECK(core::tui::test::canvasToString(fixture.dashboard.rendered())
              .contains("not opened: this terminal cannot open links"));
}

// {{{ the mouse

namespace
{
/// @return The SGR report of a press and release of the left button at the 0-based cell (@p x, @p y).
[[nodiscard]] std::string clickAt(int x, int y)
{
    return std::format("\x1b[<0;{};{}M\x1b[<0;{};{}m", x + 1, y + 1, x + 1, y + 1);
}

/// @return The SGR report of one step of the wheel at the 0-based cell (@p x, @p y).
[[nodiscard]] std::string wheelAt(int x, int y, MouseWheel direction)
{
    return std::format("\x1b[<{};{};{}M", direction == MouseWheel::Up ? 64 : 65, x + 1, y + 1);
}

/// @return The first region the last frame recorded for @p hit; fails the test when there is none.
[[nodiscard]] core::tui::Rect regionOf(Dashboard const& dashboard, Hit hit)
{
    auto const regions = dashboard.hits().regions();
    auto const found = std::ranges::find(regions, hit, &HitRegion::hit);
    REQUIRE(found != regions.end());
    return found->area;
}
} // namespace

TEST_CASE("every frame asks for the mouse in SGR encoding", "[orchestrator][view][mouse]")
{
    auto fixture = Fixture { fleetWorld() };
    CHECK(fixture.dashboard.frame().contains(MouseReportingOn));
}

TEST_CASE("the hit map covers the rows, the panes and the Focus's tabs", "[orchestrator][view][mouse]")
{
    auto fixture = Fixture { fleetWorld() };
    CHECK(fixture.dashboard.select(RunningId).empty());
    static_cast<void>(fixture.dashboard.frame());
    auto const& hits = fixture.dashboard.hits();
    auto const kinds = std::ranges::to<std::vector>(
        hits.regions() | std::views::transform([](auto const& region) { return region.hit.kind; }));
    for (auto const kind: { HitKind::FleetPane, HitKind::FocusPane, HitKind::Row, HitKind::Command })
        CHECK(std::ranges::contains(kinds, kind));
    // No two rows share a cell, and each row's position is its own.
    auto const rows =
        std::ranges::to<std::vector>(hits.regions() | std::views::filter([](auto const& region) {
                                         return region.hit.kind == HitKind::Row;
                                     }));
    for (auto const [index, row]: std::views::enumerate(rows))
        CHECK(row.hit.value == static_cast<std::size_t>(index));
}

TEST_CASE("a click selects a row and gives the Fleet the keys", "[orchestrator][view][mouse]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    auto const target = regionOf(fixture.dashboard, Hit { .kind = HitKind::Row, .value = 2 });
    CHECK(fixture.dashboard.input(clickAt(target.x + 2, target.y)).empty());
    CHECK(fixture.dashboard.interaction().selected == 2);
    CHECK(fixture.dashboard.interaction().place == Place::Fleet);
}

TEST_CASE("a double click runs the row's primary action", "[orchestrator][view][mouse]")
{
    auto fixture = Fixture { fleetWorld() };
    CHECK(fixture.dashboard.select(RunningId).empty());
    static_cast<void>(fixture.dashboard.frame());
    auto const selected = fixture.dashboard.interaction().selected;
    auto const target = regionOf(fixture.dashboard, Hit { .kind = HitKind::Row, .value = selected });
    auto const click = clickAt(target.x + 2, target.y);
    static_cast<void>(fixture.dashboard.input(click + click));
    // A working run's primary action writes to it: the message box gets the keys.
    CHECK(fixture.dashboard.interaction().place == Place::Focus);
}

TEST_CASE("a click on a tab shows it", "[orchestrator][view][mouse]")
{
    auto fixture = Fixture { fleetWorld() };
    CHECK(fixture.dashboard.select(RunningId).empty());
    static_cast<void>(fixture.dashboard.frame());
    auto const details =
        regionOf(fixture.dashboard,
                 Hit { .kind = HitKind::Command, .value = std::to_underlying(KeyCommand::DetailsTab) });
    static_cast<void>(fixture.dashboard.input(clickAt(details.x, details.y)));
    static_cast<void>(fixture.dashboard.frame());
    REQUIRE(fixture.dashboard.interaction().chosenTab.has_value());
    CHECK(fixture.dashboard.interaction().chosenTab->tab == FocusTab::Details);
}

TEST_CASE("the wheel moves the Fleet's selection and scrolls the Focus", "[orchestrator][view][mouse]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    auto const fleet = regionOf(fixture.dashboard, Hit { .kind = HitKind::FleetPane });
    auto const before = fixture.dashboard.interaction().selected;
    static_cast<void>(fixture.dashboard.input(wheelAt(fleet.x + 1, fleet.y + 1, MouseWheel::Down)));
    CHECK(fixture.dashboard.interaction().selected == before + 1);
    static_cast<void>(fixture.dashboard.input(wheelAt(fleet.x + 1, fleet.y + 1, MouseWheel::Up)));
    CHECK(fixture.dashboard.interaction().selected == before);
}

TEST_CASE("a click on a link asks to open it", "[orchestrator][view][mouse]")
{
    auto fixture = Fixture { fleetWorld() };
    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK(fixture.dashboard.input("d").empty());
    static_cast<void>(fixture.screen());
    auto const links = fixture.dashboard.rendered().hyperlinks();
    auto const link = std::ranges::find(
        links, "https://github.com/contour-terminal/contour/pull/2375", &core::tui::HyperlinkRegion::uri);
    REQUIRE(link != links.end());
    auto const actions = fixture.dashboard.input(clickAt(link->cellArea.x, link->cellArea.y));
    REQUIRE(actions.size() == 1);
    auto const* const open = std::get_if<OpenUrlAction>(&actions.front());
    REQUIRE(open != nullptr);
    CHECK(open->url == "https://github.com/contour-terminal/contour/pull/2375");
}

TEST_CASE("a click on a palette entry runs it; one beside the palette closes it",
          "[orchestrator][view][mouse]")
{
    auto fixture = Fixture { fleetWorld() };
    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK(fixture.dashboard.input("\x0b").empty());
    static_cast<void>(fixture.dashboard.frame());
    REQUIRE(fixture.dashboard.interaction().palette.has_value());
    // Beside it: closed, nothing run.
    CHECK(fixture.dashboard.input(clickAt(0, 0)).empty());
    CHECK_FALSE(fixture.dashboard.interaction().palette.has_value());

    CHECK(fixture.dashboard.input("\x0b").empty());
    static_cast<void>(fixture.dashboard.frame());
    auto const regions = fixture.dashboard.hits().regions();
    auto const entry = std::ranges::find_if(
        regions, [](HitRegion const& region) { return region.hit.kind == HitKind::PaletteEntry; });
    REQUIRE(entry != regions.end());
    static_cast<void>(fixture.dashboard.input(clickAt(entry->area.x + 2, entry->area.y)));
    CHECK_FALSE(fixture.dashboard.interaction().palette.has_value());
}

TEST_CASE("a double click on a palette entry runs it once", "[orchestrator][view][mouse]")
{
    auto fixture = Fixture { fleetWorld() };
    CHECK(fixture.dashboard.select(RunningId).empty());
    auto const entryClicked = [&](int presses) {
        CHECK(fixture.dashboard.input("").empty());
        static_cast<void>(fixture.dashboard.frame());
        REQUIRE(fixture.dashboard.interaction().palette.has_value());
        auto const regions = fixture.dashboard.hits().regions();
        auto const entry = std::ranges::find_if(
            regions, [](HitRegion const& region) { return region.hit.kind == HitKind::PaletteEntry; });
        REQUIRE(entry != regions.end());
        auto clicks = std::string {};
        for ([[maybe_unused]] auto const press: std::views::iota(0, presses))
            clicks += clickAt(entry->area.x + 2, entry->area.y);
        // One input, no frame between: the first press runs the entry and closes the palette; the second
        // lands on the cell the last frame still maps to it.
        auto actions = fixture.dashboard.input(clicks);
        CHECK_FALSE(fixture.dashboard.interaction().palette.has_value());
        return actions;
    };
    auto const once = entryClicked(1);
    CHECK(entryClicked(2) == once);
}

// }}}

// {{{ the Work view's mouse

namespace
{
/// @return The SGR report of a press and release of the right button at the 0-based cell (@p x, @p y).
[[nodiscard]] std::string rightClickAt(int x, int y)
{
    return std::format("\x1b[<2;{};{}M\x1b[<2;{};{}m", x + 1, y + 1, x + 1, y + 1);
}

/// @return The SGR report of a press of the left button alone at the 0-based cell (@p x, @p y).
[[nodiscard]] std::string pressAt(int x, int y)
{
    return std::format("\x1b[<0;{};{}M", x + 1, y + 1);
}

/// @return The SGR report of the pointer moving to the 0-based cell (@p x, @p y) with @p button held: 0 the
///         left one, 3 none.
[[nodiscard]] std::string moveAt(int button, int x, int y)
{
    return std::format("\x1b[<{};{};{}M", 32 + button, x + 1, y + 1);
}

/// @return The Work view's lines of @p fixture's dashboard, as it builds them.
[[nodiscard]] std::vector<WorkLine> workLinesOf(Fixture const& fixture, std::vector<WorkItem>& items)
{
    items = fixture.engine.workItems();
    auto const order = orderOf(items, fixture.engine.repositories(), sortOf(fixture.engine));
    return workLines(items, order, fixture.engine, fixture.dashboard.interaction().work);
}

/// @return The index of the Work line whose item has @p key (workItemKey()); fails the test when none has.
[[nodiscard]] std::size_t workLineOf(Fixture const& fixture, std::string_view key)
{
    auto items = std::vector<WorkItem> {};
    auto const lines = workLinesOf(fixture, items);
    for (auto const [at, line]: std::views::enumerate(lines))
        if (line.kind == WorkLineKind::Item && workItemKey(items[line.item]) == key)
            return static_cast<std::size_t>(at);
    FAIL("no line of " << key);
    return 0;
}

/// The key of #2375's item, Needs you's first.
constexpr auto FirstMr = "contour/!2375"sv;

/// @return The Work view of the Work world, drawn.
[[nodiscard]] std::unique_ptr<Fixture> workFixture(World const& world = workWorld(), Size size = Wide)
{
    auto fixture = std::make_unique<Fixture>(world, size);
    static_cast<void>(fixture->dashboard.frame());
    CHECK(fixture->dashboard.input("w").empty());
    static_cast<void>(fixture->dashboard.frame());
    return fixture;
}

/// @return Every region the last frame recorded of @p kind.
[[nodiscard]] std::vector<HitRegion> regionsOf(Dashboard const& dashboard, HitKind kind)
{
    return std::ranges::to<std::vector>(
        dashboard.hits().regions()
        | std::views::filter([kind](HitRegion const& region) { return region.hit.kind == kind; }));
}

/// The Work world with two workers and two tasks for a merge request.
[[nodiscard]] World twoWorkersWorld()
{
    auto world = workWorld();
    world.configJson =
        std::format(R"({{ "worktree_root": "~/worktrees", "models": ["opus", "sonnet", "haiku"],
  "workers": [ {{ "id": "a", "agent": "claude" }}, {{ "id": "b", "agent": "claude" }} ],
  "repos": {},
  "tasks": [
    {{ "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet", "worker": "a",
       "prompt": "/contour-workflows:fix-ci {{url}}" }},
    {{ "id": "review", "key": "v", "label": "address review", "applies_to": "pr", "model": "sonnet",
       "worker": "a", "prompt": "/contour-workflows:address-review {{url}}" }} ] }})",
                    WorkRepository);
    world.stored = {};
    return world;
}
} // namespace

TEST_CASE("Work mouse: a right-click on a row opens its menu; Open MR opens it",
          "[orchestrator][view][mouse][work]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    auto const row =
        regionOf(dashboard, Hit { .kind = HitKind::WorkRow, .value = workLineOf(*fixture, FirstMr) });
    CHECK(dashboard.input(rightClickAt(row.x + 30, row.y)).empty());
    auto const& menu = dashboard.interaction().work.menu;
    REQUIRE(menu.has_value());
    CHECK(menu->itemKey == FirstMr);
    CHECK(menu->at == core::tui::Point { .x = row.x + 30, .y = row.y });
    static_cast<void>(dashboard.frame());
    CHECK(regionsOf(dashboard, HitKind::MenuEntry).size() == RowMenuEntries.size());
    auto const openMr = regionOf(dashboard, Hit { .kind = HitKind::MenuEntry, .value = 1 });
    CHECK(dashboard.input(clickAt(openMr.x + 1, openMr.y))
          == std::vector<UiAction> {
              OpenUrlAction { .url = "https://github.com/example/contour/pull/2375", .label = "#2375" } });
    CHECK_FALSE(dashboard.interaction().work.menu.has_value());

    SECTION("on a ticket row, Open MR does nothing")
    {
        static_cast<void>(dashboard.frame());
        auto const ticket = regionOf(
            dashboard, Hit { .kind = HitKind::WorkRow, .value = workLineOf(*fixture, "contour/PRJ-1302") });
        CHECK(dashboard.input(rightClickAt(ticket.x + 30, ticket.y)).empty());
        static_cast<void>(dashboard.frame());
        auto const region = regionOf(dashboard, Hit { .kind = HitKind::MenuEntry, .value = 1 });
        CHECK(dashboard.input(clickAt(region.x + 1, region.y)).empty());
        REQUIRE(dashboard.interaction().work.menu.has_value());
        static_cast<void>(dashboard.frame());
        CHECK(core::tui::test::canvasToString(dashboard.rendered()).contains("Open MR"));
        // A click beside the menu closes it.
        CHECK(dashboard.input(clickAt(0, 0)).empty());
        CHECK_FALSE(dashboard.interaction().work.menu.has_value());
    }
}

TEST_CASE("Work mouse: m opens the menu under the selected row; the keys run it",
          "[orchestrator][view][mouse][work]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    auto const row =
        regionOf(dashboard, Hit { .kind = HitKind::WorkRow, .value = workLineOf(*fixture, FirstMr) });
    CHECK(dashboard.input("m").empty());
    REQUIRE(dashboard.interaction().work.menu.has_value());
    CHECK(dashboard.interaction().work.menu->at == core::tui::Point { .x = row.x + 2, .y = row.y + 1 });
    // ↓ ↓ ⏎: Open ticket.
    CHECK(dashboard.input("\x1b[B\x1b[B\r")
          == std::vector<UiAction> { OpenUrlAction { .url = "https://example.atlassian.net/browse/PRJ-1234",
                                                     .label = "PRJ-1234" } });
    CHECK_FALSE(dashboard.interaction().work.menu.has_value());
    // ⏎ on Start agent… opens the start dialog on the item.
    CHECK(dashboard.input("m\r").empty());
    REQUIRE(dashboard.interaction().startDialog.has_value());
    CHECK(dashboard.interaction().startDialog->form.item == ItemRef { ChangeRequestRef { 2375 } });
    CHECK_FALSE(dashboard.interaction().startDialog->form.session.empty());
}

TEST_CASE("Work mouse: a press on a menu entry the menu closed before does nothing",
          "[orchestrator][view][mouse][work]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    auto const row =
        regionOf(dashboard, Hit { .kind = HitKind::WorkRow, .value = workLineOf(*fixture, FirstMr) });
    CHECK(dashboard.input(rightClickAt(row.x + 30, row.y)).empty());
    static_cast<void>(dashboard.frame());
    auto const start = regionOf(dashboard, Hit { .kind = HitKind::MenuEntry, .value = 0 });
    auto const openMr = regionOf(dashboard, Hit { .kind = HitKind::MenuEntry, .value = 1 });
    // One input, no frame between: the first press chooses Open MR and closes the menu; the second lands on
    // Start agent…'s cell, which the last frame still maps.
    CHECK(dashboard.input(clickAt(openMr.x + 1, openMr.y) + clickAt(start.x + 1, start.y))
          == std::vector<UiAction> {
              OpenUrlAction { .url = "https://github.com/example/contour/pull/2375", .label = "#2375" } });
    CHECK_FALSE(dashboard.interaction().work.menu.has_value());
    CHECK_FALSE(dashboard.interaction().startDialog.has_value());
}

TEST_CASE("Work palette: fold the other groups", "[orchestrator][view][work]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    REQUIRE(dashboard.interaction().work.selectedKey == FirstMr);
    CHECK(dashboard.input("other groups").empty());
    CHECK(dashboard.input("")
          == std::vector<UiAction> { SetFoldAction { .group = "work/ready", .choice = FoldChoice::Folded },
                                     SetFoldAction { .group = "work/in-review", .choice = FoldChoice::Folded },
                                     SetFoldAction { .group = "work/no-mr", .choice = FoldChoice::Folded } });
    CHECK_FALSE(dashboard.interaction().palette.has_value());
    // The selection stays where it was.
    CHECK(dashboard.interaction().work.selectedKey == FirstMr);

    SECTION("the Agents view says where it applies")
    {
        CHECK(dashboard.input("w").empty());
        CHECK(dashboard.input("other groups").empty());
        CHECK(dashboard.interaction().notice.contains("Work view only"));
    }
}

TEST_CASE("Work mouse: a click on a heading folds; a right-click folds the others",
          "[orchestrator][view][mouse][work]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    auto const heading = regionOf(
        dashboard, Hit { .kind = HitKind::WorkHeading, .value = std::to_underlying(WorkGroup::NeedsYou) });
    CHECK(dashboard.input(clickAt(heading.x + 3, heading.y))
          == std::vector<UiAction> {
              SetFoldAction { .group = "work/needs-you", .choice = FoldChoice::Folded } });
    CHECK(
        dashboard.input(rightClickAt(heading.x + 3, heading.y))
        == std::vector<UiAction> { SetFoldAction { .group = "work/ready", .choice = FoldChoice::Folded },
                                   SetFoldAction { .group = "work/in-review", .choice = FoldChoice::Folded },
                                   SetFoldAction { .group = "work/no-mr", .choice = FoldChoice::Folded } });
}

TEST_CASE("Work mouse: the wheel scrolls the list and keeps the selection until it leaves view",
          "[orchestrator][view][mouse][work]")
{
    auto world = workWorld();
    world.prs = { .text = manyPrs(60) };
    world.issues = { .text = "[]" };
    world.answers.clear();
    world.repos = std::string { OneRepository };
    auto const fixture = workFixture(world, Medium);
    auto& dashboard = fixture->dashboard;
    auto const& work = dashboard.interaction().work;
    CHECK(dashboard.input("j").empty());
    static_cast<void>(dashboard.frame());
    REQUIRE(work.selected == 2);
    auto const row = regionOf(dashboard, Hit { .kind = HitKind::WorkRow, .value = 4 });
    CHECK(dashboard.input(wheelAt(row.x + 10, row.y, MouseWheel::Down)).empty());
    CHECK(work.scroll == 3);
    CHECK(work.selected == 3);
    static_cast<void>(dashboard.frame());
    CHECK(work.scroll == 3);
    // Back up: the selection is still shown, and stays.
    CHECK(dashboard.input(wheelAt(row.x + 10, row.y, MouseWheel::Up)).empty());
    CHECK(work.scroll == 0);
    CHECK(work.selected == 3);
}

TEST_CASE("Work mouse: the scrollbar jumps on a click and follows a drag",
          "[orchestrator][view][mouse][work]")
{
    auto world = workWorld();
    world.prs = { .text = manyPrs(60) };
    world.issues = { .text = "[]" };
    world.answers.clear();
    world.repos = std::string { OneRepository };
    auto const fixture = workFixture(world, Medium);
    auto& dashboard = fixture->dashboard;
    auto const& work = dashboard.interaction().work;
    // A heading and sixty pull requests, eighteen of them shown.
    constexpr auto End = std::size_t { 61 - 18 };
    auto const bar = regionOf(dashboard, Hit { .kind = HitKind::WorkScrollbar });
    CHECK(bar.height == 18);
    CHECK(dashboard.input(clickAt(bar.x, bar.y + bar.height - 1)).empty());
    CHECK(work.scroll == End);
    static_cast<void>(dashboard.frame());
    CHECK(work.scroll == End);

    CHECK(dashboard.input(pressAt(bar.x, bar.y)).empty());
    CHECK(work.scroll == 0);
    CHECK(dashboard.input(moveAt(0, bar.x + 3, bar.y + ((bar.height - 1) / 2))).empty());
    CHECK(work.scroll >= (End / 2) - 2);
    CHECK(work.scroll <= (End / 2) + 2);
    // Past its bottom: the end.
    CHECK(dashboard.input(moveAt(0, bar.x, bar.y + bar.height + 4)).empty());
    CHECK(work.scroll == End);
    // Released, the pointer no longer drags.
    CHECK(dashboard.input(std::format("\x1b[<0;{};{}m", bar.x + 1, bar.y + 1)).empty());
    CHECK(dashboard.input(moveAt(0, bar.x, bar.y)).empty());
    CHECK(work.scroll == End);
}

TEST_CASE("Work mouse: hover repaints only when the row under the pointer changes",
          "[orchestrator][view][mouse][work]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    auto const first = regionOf(dashboard, Hit { .kind = HitKind::WorkRow, .value = 2 });
    auto const second = regionOf(dashboard, Hit { .kind = HitKind::WorkRow, .value = 3 });
    CHECK(dashboard.input(moveAt(3, first.x + 30, first.y)).empty());
    CHECK(dashboard.interaction().work.hovered == std::optional { std::size_t { 2 } });
    CHECK_FALSE(dashboard.frame().empty());
    CHECK(dashboard.input(moveAt(3, first.x + 31, first.y)).empty());
    CHECK(dashboard.frame().empty());
    CHECK(dashboard.input(moveAt(3, second.x + 30, second.y)).empty());
    CHECK_FALSE(dashboard.frame().empty());
    CHECK(core::tui::test::canvasToString(dashboard.rendered()).contains("▏#2377"));
    // Off the rows: nothing is hovered.
    CHECK(dashboard.input(moveAt(3, 0, 0)).empty());
    CHECK_FALSE(dashboard.interaction().work.hovered.has_value());
}

TEST_CASE("Work mouse: double-click on the ticket key opens the ticket", "[orchestrator][view][mouse][work]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    auto const key =
        regionOf(dashboard, Hit { .kind = HitKind::WorkTicketLink, .value = workLineOf(*fixture, FirstMr) });
    auto const click = clickAt(key.x + 1, key.y);
    CHECK(dashboard.input(click + click)
          == std::vector<UiAction> { OpenUrlAction { .url = "https://example.atlassian.net/browse/PRJ-1234",
                                                     .label = "PRJ-1234" } });
}

TEST_CASE("Work mouse: a single click on an MR or ticket cell only selects",
          "[orchestrator][view][mouse][work]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    auto const line = workLineOf(*fixture, "contour/!2377");
    auto const mr = regionOf(dashboard, Hit { .kind = HitKind::WorkMrLink, .value = line });
    CHECK(dashboard.input(clickAt(mr.x + 1, mr.y)).empty());
    CHECK(dashboard.interaction().work.selected == line);
    CHECK(dashboard.input("k").empty());
    auto const ticket = regionOf(dashboard, Hit { .kind = HitKind::WorkTicketLink, .value = line });
    CHECK(dashboard.input(clickAt(ticket.x + 1, ticket.y)).empty());
    CHECK(dashboard.interaction().work.selected == line);

    SECTION("a double click on the MR number opens it")
    {
        auto const click = clickAt(mr.x + 2, mr.y);
        CHECK(dashboard.input(click + click)
              == std::vector<UiAction> { OpenUrlAction {
                  .url = "https://github.com/example/contour/pull/2377", .label = "#2377" } });
    }
}

TEST_CASE("Work mouse: double-click on a row opens the start dialog", "[orchestrator][view][mouse][start]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    CHECK(dashboard.input("j").empty());
    static_cast<void>(dashboard.frame());
    auto const row =
        regionOf(dashboard, Hit { .kind = HitKind::WorkRow, .value = workLineOf(*fixture, FirstMr) });
    auto const click = clickAt(row.x + row.width - 3, row.y);
    CHECK(dashboard.input(click + click).empty());
    REQUIRE(dashboard.interaction().startDialog.has_value());
    CHECK(dashboard.interaction().startDialog->form.item == ItemRef { ChangeRequestRef { 2375 } });
    CHECK_FALSE(dashboard.interaction().startDialog->form.session.empty());
}

TEST_CASE("Work mouse: the dialog's settings step under the pointer", "[orchestrator][view][mouse][start]")
{
    auto const fixture = workFixture(twoWorkersWorld());
    auto& dashboard = fixture->dashboard;
    CHECK(dashboard.input("\r").empty());
    static_cast<void>(dashboard.frame());
    auto const& dialog = dashboard.interaction().startDialog;
    REQUIRE(dialog.has_value());
    CHECK(dialog->form.worker == 0);

    // A worker chip picks it.
    auto const chip = regionOf(dashboard, Hit { .kind = HitKind::WorkerChip, .value = 1 });
    CHECK(dashboard.input(clickAt(chip.x + 1, chip.y)).empty());
    CHECK(dialog->form.worker == 1);
    CHECK(dialog->setting == FormField::Worker);
    static_cast<void>(dashboard.frame());

    // The wheel over Model steps it.
    auto const model = regionOf(
        dashboard, Hit { .kind = HitKind::DialogSetting, .value = std::to_underlying(FormField::Model) });
    auto const modelBefore = dialog->form.model;
    CHECK(dashboard.input(wheelAt(model.x + 1, model.y, MouseWheel::Down)).empty());
    CHECK(dialog->form.model != modelBefore);
    CHECK(dialog->setting == FormField::Model);
    static_cast<void>(dashboard.frame());

    // A right-click on Task's value steps it back; a click on its value steps it on.
    auto const task =
        regionOf(dashboard,
                 Hit { .kind = HitKind::DialogStep, .value = (2 * std::to_underlying(FormField::Task)) + 1 });
    auto const taskBefore = dialog->form.task;
    CHECK(dashboard.input(rightClickAt(task.x + 1, task.y)).empty());
    CHECK(dialog->form.task != taskBefore);
    CHECK(dialog->setting == FormField::Task);
    static_cast<void>(dashboard.frame());
    CHECK(dashboard.input(clickAt(task.x + 1, task.y)).empty());
    CHECK(dialog->form.task == taskBefore);
    static_cast<void>(dashboard.frame());

    // A double click on a stepper steps twice and starts nothing.
    auto const modelOn = regionOf(
        dashboard,
        Hit { .kind = HitKind::DialogStep, .value = (2 * std::to_underlying(FormField::Model)) + 1 });
    auto const modelAt = dialog->form.model;
    auto const twice = clickAt(modelOn.x + 1, modelOn.y);
    CHECK(dashboard.input(twice + twice).empty());
    REQUIRE(dialog.has_value());
    CHECK(dialog->form.model == (modelAt + 2) % 3);
    static_cast<void>(dashboard.frame());

    // A click on a label picks the setting.
    auto const note = regionOf(
        dashboard, Hit { .kind = HitKind::DialogSetting, .value = std::to_underlying(FormField::Note) });
    CHECK(dashboard.input(clickAt(note.x, note.y)).empty());
    CHECK(dialog->setting == FormField::Note);

    // A click outside closes it.
    CHECK(dashboard.input(clickAt(0, 0)).empty());
    CHECK_FALSE(dialog.has_value());
}

TEST_CASE("Work mouse: the n box's settings step under the pointer", "[orchestrator][view][mouse][start]")
{
    auto const fixture = workFixture(twoWorkersWorld());
    auto& dashboard = fixture->dashboard;
    CHECK(dashboard.input("n").empty());
    static_cast<void>(dashboard.frame());
    auto const& box = dashboard.interaction().taskBox;
    REQUIRE(box.has_value());
    auto const worker = regionOf(
        dashboard,
        Hit { .kind = HitKind::DialogStep, .value = (2 * std::to_underlying(TaskBoxField::Worker)) + 1 });
    auto const before = box->form.worker;
    CHECK(dashboard.input(clickAt(worker.x + 1, worker.y)).empty());
    CHECK(box->focus == TaskBoxField::Worker);
    CHECK(box->form.worker != before);
    static_cast<void>(dashboard.frame());
    auto const text = regionOf(
        dashboard, Hit { .kind = HitKind::DialogSetting, .value = std::to_underlying(TaskBoxField::Text) });
    CHECK(dashboard.input(clickAt(text.x, text.y)).empty());
    CHECK(box->focus == TaskBoxField::Text);
}

TEST_CASE("Work mouse: a click on Agents/Work in the top bar switches views",
          "[orchestrator][view][mouse][work]")
{
    auto fixture = Fixture { workWorld() };
    auto& dashboard = fixture.dashboard;
    static_cast<void>(dashboard.frame());
    auto const work =
        regionOf(dashboard, Hit { .kind = HitKind::ViewName, .value = std::to_underlying(View::Work) });
    CHECK(dashboard.input(clickAt(work.x, work.y)).empty());
    CHECK(dashboard.interaction().view == View::Work);
    static_cast<void>(dashboard.frame());
    // The shown view's name does nothing.
    CHECK(dashboard.input(clickAt(work.x + 1, work.y)).empty());
    CHECK(dashboard.interaction().view == View::Work);
    auto const agents =
        regionOf(dashboard, Hit { .kind = HitKind::ViewName, .value = std::to_underlying(View::Agents) });
    CHECK(dashboard.input(clickAt(agents.x, agents.y)).empty());
    CHECK(dashboard.interaction().view == View::Agents);
}

TEST_CASE("Work mouse: every hint in the bar runs its action", "[orchestrator][view][mouse][work]")
{
    auto const fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    CHECK(core::tui::test::canvasToString(dashboard.rendered()).contains("⏎ start · n new task"));
    auto const hint = [&](KeyCommand command) {
        return regionOf(dashboard, Hit { .kind = HitKind::Command, .value = std::to_underlying(command) });
    };
    auto const start = hint(KeyCommand::Enter);
    CHECK(dashboard.input(clickAt(start.x, start.y)).empty());
    REQUIRE(dashboard.interaction().startDialog.has_value());
    CHECK(dashboard.input("\x1b").empty());
    static_cast<void>(fixture->escapeTimesOut());
    CHECK_FALSE(dashboard.interaction().startDialog.has_value());
    static_cast<void>(dashboard.frame());
    auto const task = hint(KeyCommand::NewTask);
    CHECK(dashboard.input(clickAt(task.x + 1, task.y)).empty());
    CHECK(dashboard.interaction().taskBox.has_value());
    CHECK(dashboard.input("\x1b").empty());
    static_cast<void>(fixture->escapeTimesOut());
    static_cast<void>(dashboard.frame());
    // Every Work view hint the bar draws stands for its key.
    for (auto const& region: regionsOf(dashboard, HitKind::Command))
        if (region.area.y == Wide.height - 1)
            CHECK((!workKeyNameOf(static_cast<KeyCommand>(region.hit.value)).empty()
                   || static_cast<KeyCommand>(region.hit.value) == KeyCommand::Palette));
    auto const palette = hint(KeyCommand::Palette);
    CHECK(dashboard.input(clickAt(palette.x, palette.y)).empty());
    CHECK(dashboard.interaction().palette.has_value());
}

TEST_CASE("Row menu: a poll that drops its item closes it", "[orchestrator][view][work]")
{
    auto fixture = workFixture();
    auto& dashboard = fixture->dashboard;
    CHECK(dashboard.input("m").empty());
    REQUIRE(dashboard.interaction().work.menu.has_value());
    static_cast<void>(dashboard.frame());
    // The next poll lists every pull request but #2375.
    auto prs = readFixture(FixtureRoot::Testdata, "github/prs-work.json");
    auto const second = prs.find(",[2376,");
    REQUIRE(second != std::string::npos);
    fixture->world.prs = { .text = "[" + prs.substr(second + 1) };
    fixture->poll();
    static_cast<void>(dashboard.frame());
    CHECK_FALSE(dashboard.interaction().work.menu.has_value());
    CHECK_FALSE(core::tui::test::canvasToString(dashboard.rendered()).contains("Open ticket"));
}

// }}}

TEST_CASE("a URL drawn is a hyperlink", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK(fixture.dashboard.input("d").empty());
    static_cast<void>(fixture.screen());
    auto const links = fixture.dashboard.rendered().hyperlinks();
    CHECK(std::ranges::any_of(links, [](core::tui::HyperlinkRegion const& link) {
        return link.uri == "https://github.com/contour-terminal/contour/pull/2375";
    }));
}

// }}}
// {{{ input

TEST_CASE("key input", "[orchestrator][view]")
{
    auto const withRun = World { .prs = { "github/prs-empty.json" },
                                 .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }) } };
    auto const withKept = World {
        .prs = { "github/prs-empty.json" },
        .stored = { finishedRun(KeptId, IssueRef { 2356 }, Fate::Kept, Seen::New, "issue") },
    };
    auto const outsideRun = World {
        .prs = { "github/prs-empty.json" },
        .stored = { outsideConfiguration(storedRun(Running {}, RunningId, ChangeRequestRef { 2375 })) },
    };
    auto const outsideKept = World {
        .prs = { "github/prs-empty.json" },
        .stored = { outsideConfiguration(
            finishedRun(KeptId, IssueRef { 2356 }, Fate::Kept, Seen::Opened, "issue")) },
    };
    auto const fleet = fleetWorld();

    struct Case
    {
        std::string_view name = {};
        World const* world = nullptr;
        Size size = {};
        std::string_view keys = {};
        Interaction expected = {};
        std::vector<UiAction> actions = {};
    };
    auto const cases = std::vector<Case> {
        { .name = "down", .world = &fleet, .size = Wide, .keys = "\x1b[B", .expected = { .selected = 1 } },
        { .name = "j", .world = &fleet, .size = Wide, .keys = "j", .expected = { .selected = 1 } },
        { .name = "j j k", .world = &fleet, .size = Wide, .keys = "jjk", .expected = { .selected = 1 } },
        { .name = "up at the top",
          .world = &fleet,
          .size = Wide,
          .keys = "\x1b[A",
          .expected = { .selected = 0 } },
        { .name = "end", .world = &fleet, .size = Wide, .keys = "\x1b[F", .expected = { .selected = 7 } },
        { .name = "home", .world = &fleet, .size = Wide, .keys = "jj\x1b[H", .expected = { .selected = 0 } },
        { .name = "filter",
          .world = &fleet,
          .size = Wide,
          .keys = "/2375",
          .expected = { .filtering = Filtering::Typing, .filter = "2375" } },
        { .name = "filter kept",
          .world = &fleet,
          .size = Wide,
          .keys = "/2375\r",
          .expected = { .filter = "2375" } },
        { .name = "filter dropped", .world = &fleet, .size = Wide, .keys = "/2375\x1b", .expected = {} },
        { .name = "tab to the Focus",
          .world = &fleet,
          .size = Medium,
          .keys = "\t",
          .expected = { .place = Place::Focus } },
        { .name = "tab and back", .world = &fleet, .size = Narrow, .keys = "\t\t", .expected = {} },
        { .name = "tab, esc", .world = &fleet, .size = Narrow, .keys = "\t\x1b", .expected = {} },
        { .name = "palette",
          .world = &fleet,
          .size = Wide,
          .keys = "?",
          .expected = { .palette = PaletteState {} } },
        { .name = "palette closed, its query kept",
          .world = &fleet,
          .size = Wide,
          .keys = "\x0bref\x1b",
          .expected = { .paletteQuery = "ref" } },
        { .name = "refresh, from the palette",
          .world = &fleet,
          .size = Wide,
          .keys = "\x0brefresh\r",
          .expected = {},
          .actions = { RefreshNowAction {} } },
        { .name = "b chooses the Board",
          .world = &withRun,
          .size = Wide,
          .keys = "b",
          .expected = { .selected = 0,
                        .chosenTab = TabChoice { .run = std::string { RunningId },
                                                 .state = "working",
                                                 .tab = FocusTab::Board } } },
        { .name = "r reveals the agent's tab",
          .world = &withRun,
          .size = Wide,
          .keys = "r",
          .expected = {},
          .actions = { RevealAgentAction { .run = std::string { RunningId } } } },
        { .name = "e on an active run asks",
          .world = &withRun,
          .size = Wide,
          .keys = "e",
          .expected = { .pendingEnd = std::string { RunningId } } },
        { .name = "e, e on an active run",
          .world = &withRun,
          .size = Wide,
          .keys = "ee",
          .expected = {},
          .actions = { EndRunAction { .run = std::string { RunningId } } } },
        { .name = "e, k keeps the run and does not move",
          .world = &withRun,
          .size = Wide,
          .keys = "ek",
          .expected = {} },
        { .name = "e, esc keeps the run", .world = &withRun, .size = Wide, .keys = "e\x1b", .expected = {} },
        { .name = "enter writes to a live run",
          .world = &withRun,
          .size = Wide,
          .keys = "\r",
          .expected = { .place = Place::Focus } },
        { .name = "delete while typing a filter",
          .world = &withKept,
          .size = Wide,
          .keys = "/\x1b[3~",
          .expected = { .filtering = Filtering::Typing } },
        { .name = "x on a kept entry",
          .world = &withKept,
          .size = Wide,
          .keys = "x",
          .expected = {},
          .actions = { RetryRemovalAction { .run = std::string { KeptId } } } },
        { .name = "enter on a kept entry retries its removal",
          .world = &withKept,
          .size = Wide,
          .keys = "\r",
          .expected = {},
          .actions = { RetryRemovalAction { .run = std::string { KeptId } } } },
        { .name = "x on a kept entry outside the configuration",
          .world = &outsideKept,
          .size = Wide,
          .keys = "x",
          .expected = { .notice = "x retry removal · its removal cannot be tried again now" } },
        { .name = "e, e on an active run outside the configuration",
          .world = &outsideRun,
          .size = Wide,
          .keys = "ee",
          .expected = {},
          .actions = { EndRunAction { .run = std::string { RunningId } } } },
        { .name = "delete on a finished entry outside the configuration",
          .world = &outsideKept,
          .size = Wide,
          .keys = "\x1b[3~",
          .expected = {},
          .actions = { DismissAction { .run = std::string { KeptId } } } },
        { .name = "delete on a finished entry",
          .world = &withKept,
          .size = Wide,
          .keys = "\x1b[3~",
          .expected = {},
          .actions = { DismissAction { .run = std::string { KeptId } } } },
    };
    for (auto const& row: cases)
    {
        CAPTURE(row.name);
        auto fixture = Fixture { *row.world, row.size };
        static_cast<void>(fixture.dashboard.frame());
        CHECK(fixture.dashboard.input(row.keys) == row.actions);
        fixture.escapeTimesOut();
        CHECK(fixture.dashboard.interaction() == row.expected);
    }
}

TEST_CASE("n opens the box; enter on + new task too", "[orchestrator][view]")
{
    auto fixture = Fixture { World {} };
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.interaction().taskBox.has_value());
    CHECK(fixture.dashboard.input("\x1b").empty());
    fixture.escapeTimesOut();
    REQUIRE_FALSE(fixture.dashboard.interaction().taskBox.has_value());
    // `+ new task` is the only row, selected.
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(fixture.dashboard.interaction().taskBox.has_value());

    // Unconfigured, `n` says what is missing.
    auto unconfigured = Fixture { World { .configJson = "{}" } };
    CHECK(unconfigured.dashboard.input("n").empty());
    CHECK_FALSE(unconfigured.dashboard.interaction().taskBox.has_value());
    CHECK(unconfigured.screen().contains("not configured"));
}

TEST_CASE("a key in an open popup sends what changed and not the whole tab", "[orchestrator][view]")
{
    auto fixture = Fixture { World {} };
    std::ignore = fixture.dashboard.frame();
    CHECK(fixture.dashboard.input("n").empty());
    std::ignore = fixture.dashboard.frame();
    // A text typed shows what it starts: the popup grows, once.
    CHECK(fixture.dashboard.input("x").empty());
    auto const opened = fixture.dashboard.frame();
    REQUIRE_FALSE(opened.empty());
    // The popup stays where it is: the frame carries the letter typed, no repaint.
    CHECK(fixture.dashboard.input("y").empty());
    auto const typed = fixture.dashboard.frame();
    REQUIRE_FALSE(typed.empty());
    CHECK(typed.size() * 2 < opened.size());
}

TEST_CASE("enter on a finished run runs it again through the start dialog", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(NewId).empty());
    // Its item is no longer listed: the dialog does not open, and the action bar says why.
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK_FALSE(fixture.dashboard.interaction().startDialog.has_value());
    CHECK(fixture.screen().contains("#2366 is not listed any more"));
}

TEST_CASE("an ESC that ends an input waits for the rest of its sequence", "[orchestrator][view]")
{
    SECTION("ESC, then [B: one Down")
    {
        auto fixture = Fixture { fleetWorld() };
        static_cast<void>(fixture.dashboard.frame());
        CHECK(fixture.dashboard.input("\x1b").empty());
        REQUIRE(fixture.dashboard.pendingEscapeDeadline().has_value());
        fixture.clock.advance(std::chrono::milliseconds { 10 });
        static_cast<void>(fixture.dashboard.frame());
        CHECK(fixture.dashboard.input("[B").empty());
        CHECK(fixture.dashboard.interaction() == Interaction { .selected = 1 });
        CHECK(!fixture.dashboard.pendingEscapeDeadline().has_value());
    }
    SECTION("ESC alone, then its deadline: the Escape key")
    {
        auto fixture = Fixture { fleetWorld() };
        CHECK(fixture.dashboard.input("?").empty());
        CHECK(fixture.dashboard.input("\x1b").empty());
        CHECK(fixture.dashboard.interaction() == Interaction { .palette = PaletteState {} });
        auto const deadline = fixture.dashboard.pendingEscapeDeadline();
        REQUIRE(deadline.has_value());
        CHECK(*deadline == fixture.clock.now() + std::chrono::milliseconds { 50 });
        fixture.clock.advance(std::chrono::milliseconds { 60 });
        static_cast<void>(fixture.dashboard.frame());
        CHECK(fixture.dashboard.interaction() == Interaction {});
        CHECK(!fixture.dashboard.pendingEscapeDeadline().has_value());
    }
    SECTION("ESC, then j: Alt+j, which does nothing")
    {
        auto fixture = Fixture { fleetWorld() };
        CHECK(fixture.dashboard.input("j").empty());
        CHECK(fixture.dashboard.input("\x1b").empty());
        CHECK(fixture.dashboard.input("j").empty());
        fixture.escapeTimesOut();
        CHECK(fixture.dashboard.interaction() == Interaction { .selected = 1 });
    }
    SECTION("ESC, then j after the deadline: Escape, then j")
    {
        auto fixture = Fixture { fleetWorld() };
        CHECK(fixture.dashboard.input("?").empty());
        CHECK(fixture.dashboard.input("\x1b").empty());
        fixture.clock.advance(std::chrono::milliseconds { 60 });
        CHECK(fixture.dashboard.input("j").empty());
        CHECK(fixture.dashboard.interaction() == Interaction { .selected = 1 });
    }
}

TEST_CASE("an escape sequence split across two input calls", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.input("\x1b[").empty());
    CHECK(fixture.dashboard.interaction() == Interaction {});
    CHECK(fixture.dashboard.input("B").empty());
    CHECK(fixture.dashboard.interaction() == Interaction { .selected = 1 });
}

TEST_CASE("a hint line is cut between hints, never inside one", "[orchestrator][view]")
{
    static constexpr auto Hints = std::array { "enter open"sv, "d dispatch"sv, "esc back"sv };
    CHECK(joinHints(Hints, 100) == "enter open · d dispatch · esc back");
    CHECK(joinHints(Hints, 30) == "enter open · d dispatch");
    CHECK(joinHints(Hints, 22) == "enter open");
    CHECK(joinHints(Hints, 5).empty());
}

TEST_CASE("the action bar offers a run's keys only where they apply", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    auto const live = fixture.screen();
    CHECK(live.contains("e end"));
    CHECK_FALSE(live.contains("del dismiss"));
    CHECK(fixture.dashboard.select(NewId).empty());
    auto const finished = fixture.screen();
    CHECK_FALSE(finished.contains("e end"));
    CHECK(finished.contains("del dismiss"));
}

// }}}
// {{{ frames

TEST_CASE("the rows are built once per input, not once per key", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    fixture.dashboard.engineChanged();
    auto const before = fixture.dashboard.rowBuilds();
    CHECK(fixture.dashboard.input(std::string(25, 'j') + std::string(25, 'k')).empty());
    CHECK(fixture.dashboard.rowBuilds() == before + 1);
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.rowBuilds() == before + 1);
    // A pause that moves no age on builds nothing: these ages count in minutes.
    fixture.clock.advance(std::chrono::seconds { 2 });
    CHECK(fixture.dashboard.input("j").empty());
    CHECK(fixture.dashboard.rowBuilds() == before + 1);
    // An age that moved on builds them again at the next frame that draws.
    fixture.clock.advance(std::chrono::minutes { 61 });
    CHECK(fixture.dashboard.input("j").empty());
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.rowBuilds() == before + 2);
}

TEST_CASE("the selected row is drawn in the selection's style", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    // Row 0 is the top bar, row 1 the Fleet's frame, row 2 the first heading; the first run is on rows 3
    // and 4.
    static_cast<void>(fixture.screen());
    auto const selection = styleOf(dashboardTheme(), Role::Selection).bg;
    auto const isSelected = [&](int row) {
        return core::tui::test::styleAt(fixture.dashboard.rendered(), row, 3).bg == selection;
    };
    CHECK(!isSelected(2));
    CHECK(isSelected(3));
    CHECK(isSelected(4));
    CHECK(!isSelected(5));
    CHECK(fixture.dashboard.input("j").empty());
    static_cast<void>(fixture.screen());
    CHECK(!isSelected(3));
    CHECK(isSelected(5));
}

TEST_CASE("a second frame with nothing changed emits nothing", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    CHECK(!fixture.dashboard.frame().empty());
    CHECK(fixture.dashboard.frame().empty());
    // Neither does a key that changes nothing, or a resize to the size the tab has.
    CHECK(fixture.dashboard.input("k").empty());
    fixture.dashboard.resize(Wide);
    CHECK(fixture.dashboard.frame().empty());
    // A key that moves the selection does.
    CHECK(fixture.dashboard.input("j").empty());
    CHECK(!fixture.dashboard.frame().empty());
    CHECK(fixture.dashboard.frame().empty());
    // So does a change of the engine's lists.
    fixture.dashboard.engineChanged();
    CHECK(!fixture.dashboard.frame().empty());
}

// }}}

TEST_CASE("select picks a run's row, dropping a filter that hides it", "[orchestrator][view]")
{
    constexpr auto BlockedId = "pr-2379-20261113-151500"sv;
    auto fixture =
        Fixture { World { .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                                      storedRun(Blocked {}, BlockedId, ChangeRequestRef { 2379 }) } } };
    static_cast<void>(fixture.dashboard.frame());
    // The run that waits for the user comes first.
    CHECK(fixture.dashboard.selectedRun() == BlockedId);

    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK(fixture.dashboard.selectedRun() == RunningId);
    CHECK(!fixture.dashboard.frame().empty());

    // A filter that hides the row is dropped; an unknown run selects nothing else.
    CHECK(fixture.dashboard.input("/2379\r").empty());
    REQUIRE(fixture.dashboard.interaction().filter == "2379");
    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK(fixture.dashboard.selectedRun() == RunningId);
    CHECK(fixture.dashboard.interaction().filter.empty());
    CHECK(fixture.dashboard.select("no-such-run").empty());
    CHECK(fixture.dashboard.selectedRun() == RunningId);

    // An end that waits for its confirmation is dropped: it was asked of another row.
    CHECK(fixture.dashboard.input("e").empty());
    REQUIRE(fixture.dashboard.interaction().pendingEnd == RunningId);
    CHECK(fixture.dashboard.select(BlockedId).empty());
    CHECK(fixture.dashboard.interaction().pendingEnd.empty());
}

TEST_CASE("an agent's age is drawn again when it moves on", "[orchestrator][view]")
{
    auto const at = [](uint64_t sinceStartMs) {
        return core::SteadyTimePoint { std::chrono::milliseconds { StartMonoMs + sinceStartMs } };
    };
    auto const engine =
        bootEngine(World { .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }) } });
    auto clock = core::ManualClock { at(45'000) };
    auto ids = CountingUuids {};
    auto dashboard =
        Dashboard { engine, dashboardTheme(), clock, Wide, ids, std::string { DashboardFixture::Tab } };
    REQUIRE_FALSE(dashboard.frame().empty());
    CHECK(core::tui::test::canvasToString(dashboard.rendered()).contains("working 45s"));
    CHECK(dashboard.nextRepaintDue() == at(46'000));

    // The engine does not change; the age does.
    clock.advance(std::chrono::seconds { 15 });
    CHECK_FALSE(dashboard.frame().empty());
    CHECK(core::tui::test::canvasToString(dashboard.rendered()).contains("working 1m"));
    CHECK(dashboard.nextRepaintDue() == at(120'000));

    // Within the minute it shows, nothing is drawn.
    clock.advance(std::chrono::seconds { 30 });
    CHECK(dashboard.frame().empty());
}

TEST_CASE("x is inert on a run outside the configuration", "[orchestrator][view]")
{
    auto fixture = Fixture { World { .prs = { "github/prs-empty.json" },
                                     .stored = { outsideConfiguration(finishedRun(
                                         KeptId, IssueRef { 2356 }, Fate::Kept, Seen::Opened, "issue")) } } };
    static_cast<void>(fixture.dashboard.frame());
    REQUIRE(fixture.dashboard.selectedRun() == KeptId);
    CHECK(fixture.dashboard.input("x").empty());
    CHECK(fixture.dashboard.input("xx").empty());
    CHECK_FALSE(fixture.screen().contains("tries the removal again"));
}

TEST_CASE("the e prompt answered no does nothing", "[orchestrator][view]")
{
    auto fixture =
        Fixture { World { .prs = { "github/prs-empty.json" },
                          .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }) } } };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.input("e").empty());
    REQUIRE(fixture.dashboard.interaction().pendingEnd == RunningId);
    CHECK(fixture.screen().contains("end pr-2375? e again to end · esc keeps it"));
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.interaction().pendingEnd.empty());
    CHECK_FALSE(fixture.dashboard.interaction().taskBox.has_value());
    // Asked once, answered: a second e asks again rather than ending it.
    CHECK(fixture.dashboard.input("e").empty());
    CHECK(fixture.dashboard.interaction().pendingEnd == RunningId);
}

TEST_CASE("a running agent with no status after 30 s hints at a trust prompt", "[orchestrator][view]")
{
    auto const at = [](uint64_t sinceStartMs) {
        return core::SteadyTimePoint { std::chrono::milliseconds { StartMonoMs + sinceStartMs } };
    };
    auto engine = bootEngine(World { .prs = { "github/prs-empty.json" }, .stored = { unreportedRun() } });
    auto const statusAt = [&](uint64_t sinceStartMs) {
        auto const rows = buildRows(engine, StartMonoMs + sinceStartMs);
        auto const row = std::ranges::find(rows, RunningId, &Row::run);
        REQUIRE(row != rows.end());
        return *row;
    };
    auto const hint = DetailLine { .label = "",
                                   .value = "may be waiting for a folder-trust prompt in its tab",
                                   .role = Role::Warning };

    // 29 s: no hint yet; 30 s: the row points at the tab.
    CHECK(statusAt(29'000).status == "starting 29s");
    CHECK(!std::ranges::contains(statusAt(29'000).details, hint));
    CHECK(statusAt(30'000).status == "waiting? see its tab");
    CHECK(statusAt(30'000).statusRole == Role::Warning);
    CHECK(std::ranges::contains(statusAt(30'000).details, hint));
    // Still endable: the hint answers nothing.
    CHECK(std::ranges::contains(statusAt(30'000).offers, KeyCommand::EndRun));

    // The dashboard draws it at 30 s on its own, asking the engine nothing.
    auto clock = core::ManualClock { at(29'500) };
    auto ids = CountingUuids {};
    auto dashboard =
        Dashboard { engine, dashboardTheme(), clock, Wide, ids, std::string { DashboardFixture::Tab } };
    REQUIRE_FALSE(dashboard.frame().empty());
    CHECK_FALSE(core::tui::test::canvasToString(dashboard.rendered()).contains("waiting? see its tab"));
    CHECK(dashboard.nextRepaintDue() == at(30'000));
    clock.advance(std::chrono::milliseconds { 500 });
    CHECK_FALSE(dashboard.frame().empty());
    CHECK(core::tui::test::canvasToString(dashboard.rendered()).contains("waiting? see its tab"));

    // The agent reports: the run records it, and the hint is gone.
    static_cast<void>(engine.step(PanesListed { .panes = { PaneFact { .pane = std::format("p-{}", RunningId),
                                                                      .tag = std::string { RunningId },
                                                                      .status = AgentStatus::Working,
                                                                      .message = {},
                                                                      .program = ProgramLiveness::Alive } },
                                                .requestedMonoMs = StartMonoMs + 31'000,
                                                .listing = PaneListing::Tagged }));
    REQUIRE(engine.runs().front().lastStatus == AgentStatus::Working);
    CHECK(statusAt(31'000).status == "working 31s");
    CHECK(!std::ranges::contains(statusAt(31'000).details, hint));
}

TEST_CASE("an orphan's row is read-only: its Focus shows its details", "[orchestrator][view]")
{
    constexpr auto Command =
        R"(git -C /home/u/projects/contour worktree remove '/home/u/worktrees/contour/it'\''s a test')"sv;
    auto fixture = Fixture { orphansWorld(), Wide };
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.input("\x1b[Hj"sv).empty());
    // No key acts on it: nothing is asked, nothing reaches the engine.
    for (auto const key: { "e"sv, "x"sv, "\x1b[3~"sv, "r"sv, "o"sv })
    {
        CAPTURE(key);
        CHECK(fixture.dashboard.input(key).empty());
        CHECK(!fixture.screen().contains("e again"));
    }
    // Beside the Fleet the Focus is too narrow for the line: it is cut, never wrapped. Given the keys, the
    // Focus takes the whole tab, and shows it whole.
    CHECK(!fixture.screen().contains(Command));
    CHECK(fixture.dashboard.input("\r"sv).empty());
    auto const page = fixture.screen();
    CHECK(page.contains(Command));
    CHECK(!page.contains("LEFTOVER WORKTREES"));
    static_cast<void>(fixture.dashboard.input("\x1b"sv));
    fixture.escapeTimesOut();
    CHECK(fixture.screen().contains("LEFTOVER WORKTREES"));
}

// }}}
// {{{ attention

namespace
{

constexpr auto OtherId = "pr-2380-20261113-151000"sv;

/// Two agents at work: pr-2375, and pr-2380, which the attention tests select while watching the other.
[[nodiscard]] World twoWorking()
{
    return World { .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                               storedRun(Running {}, OtherId, ChangeRequestRef { 2380 }) } };
}

/// Lists both agents' tabs, pr-2375's agent reporting @p status, and tells the dashboard.
void report(Fixture& fixture, AgentStatus status)
{
    auto const pane = [](std::string_view run, AgentStatus reported) {
        return PaneFact { .pane = std::format("p-{}", run),
                          .tag = std::string { run },
                          .status = reported,
                          .message = {},
                          .program = ProgramLiveness::Alive };
    };
    fixture.clock.advance(std::chrono::seconds { 1 });
    static_cast<void>(fixture.engine.step(
        PanesListed { .panes = { pane(RunningId, status), pane(OtherId, AgentStatus::Working) },
                      .requestedMonoMs = fixture.monoMs(),
                      .listing = PaneListing::Tagged }));
    fixture.dashboard.engineChanged();
}

/// @return A transcript page of @p count entries.
[[nodiscard]] ProcessOutcome pageOfEntries(int count)
{
    auto entries = std::string {};
    for (auto const index: std::views::iota(0, count))
        entries +=
            std::format(R"({}{{"kind":"assistant","time":"2026-11-19T18:00:00.000Z","text":"entry {}"}})",
                        index == 0 ? "" : ",",
                        index);
    return printed(
        std::format(R"({{"format":1,"session":"00000000-0000-4000-8000-000000000001",)"
                    R"("writtenBy":"2.1.328","page":1,"pages":1,"notUnderstood":0,"entries":[{}]}})",
                    entries));
}

} // namespace

TEST_CASE("working rows turn a spinner on the redraws that happen anyway", "[orchestrator][view][attention]")
{
    auto fixture = Fixture { twoWorking() };
    auto const first = fixture.screen();
    CHECK(first.contains("⠋ pr-2375"));
    auto const due = fixture.dashboard.nextRepaintDue();
    // Time alone draws nothing: the spinner asks for no wake of its own.
    fixture.clock.advance(std::chrono::milliseconds { 200 });
    CHECK(fixture.dashboard.frame().empty());
    CHECK(fixture.dashboard.nextRepaintDue() == due);
    // A redraw turns it a frame, every working row alike.
    CHECK(fixture.dashboard.input("j").empty());
    auto const turned = fixture.screen();
    CHECK(turned.contains("⠙ pr-2375"));
    CHECK(turned.contains("⠙ pr-2380"));
    // At most a frame per interval, however often it redraws.
    CHECK(fixture.dashboard.input("k").empty());
    CHECK(fixture.screen().contains("⠙ pr-2375"));
}

TEST_CASE("a run that enters Needs you is named on the action bar until selected",
          "[orchestrator][view][attention]")
{
    auto fixture = Fixture { twoWorking() };
    CHECK(fixture.dashboard.select(OtherId).empty());
    CHECK(!fixture.screen().contains("needs you · home"));

    // pr-2375 asks: its name on the action bar, and the pill counts it; the selection stays where it was.
    report(fixture, AgentStatus::Asking);
    auto const asked = fixture.screen();
    CHECK(asked.contains("pr-2375 needs you · home"));
    CHECK(asked.contains("● 1 need you"));
    CHECK(fixture.dashboard.selectedRun() == OtherId);

    // Selected, it is no longer named; asking to waiting is no entry, so it is not named again.
    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK(!fixture.screen().contains("needs you · home"));
    CHECK(fixture.dashboard.select(OtherId).empty());
    report(fixture, AgentStatus::Waiting);
    CHECK(!fixture.screen().contains("needs you · home"));

    // Leaving Needs you takes the name off the bar too.
    report(fixture, AgentStatus::Working);
    CHECK(!fixture.screen().contains("needs you · home"));
    report(fixture, AgentStatus::Error);
    CHECK(fixture.screen().contains("pr-2375 needs you · home"));
    report(fixture, AgentStatus::Working);
    CHECK(!fixture.screen().contains("needs you · home"));
}

TEST_CASE("a run with news since the user looked carries a dot until selected",
          "[orchestrator][view][attention]")
{
    auto fixture = Fixture { twoWorking() };
    CHECK(fixture.dashboard.select(OtherId).empty());
    CHECK(!fixture.screen().contains("•"));
    report(fixture, AgentStatus::Asking);
    CHECK(fixture.screen().contains("pr-2375 · fix CI •"));
    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK(!fixture.screen().contains("pr-2375 · fix CI •"));
    // Seen, it stays seen when the selection moves on.
    CHECK(fixture.dashboard.select(OtherId).empty());
    CHECK(!fixture.screen().contains("pr-2375 · fix CI •"));
}

TEST_CASE("a Focus tab whose content grew while another was shown carries a dot",
          "[orchestrator][view][attention]")
{
    auto fixture = Fixture { twoWorking() };
    CHECK(fixture.dashboard.select(RunningId).empty());
    // A working run opens on its Board; its transcript, first read, is seen as it is.
    static_cast<void>(fixture.settle(Answers { .transcript = pageOfEntries(2) }));
    CHECK(!fixture.screen().contains("Transcript •"));
    // Read again five seconds on, it grew.
    fixture.clock.advance(std::chrono::seconds { 5 });
    REQUIRE(fixture.settle(Answers { .transcript = pageOfEntries(3) }).size() == 1);
    CHECK(fixture.screen().contains("Transcript •"));
    // Shown, it is seen.
    CHECK(fixture.dashboard.input("t").empty());
    CHECK(!fixture.screen().contains("Transcript •"));
}

// }}}
