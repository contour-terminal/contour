// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/Responsive.hpp>
#include <core/tui/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Stats.hpp>
#include <orchestrator/view/Band.hpp>
#include <orchestrator/view/BoardMarkdown.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/Fleet.hpp>
#include <orchestrator/view/Focus.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/Theme.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

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
        .paneMessages = { { std::string { WaitingId }, "Waiting for Build pace" } },
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
        // The run that asks opens on its transcript.
        Scene { .name = "asking", .world = fleetWorld(), .sizes = { Wide } },
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
                CHECK(fixture.dashboard.input(scene.keys).empty());
                fixture.settle(sceneAnswers());
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
                                        "new agent" });
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

TEST_CASE("o copies the link with OSC 52 in the next frame", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.input("o").empty());
    auto const bytes = fixture.dashboard.frame();
    CHECK(bytes.contains("\x1b]52;c;"));
    CHECK(core::tui::test::canvasToString(fixture.dashboard.rendered()).contains("copied #2375's link"));
}

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

TEST_CASE("n opens the launcher; enter on + new agent too", "[orchestrator][view]")
{
    auto fixture = Fixture { World {} };
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.launcher().has_value());
    CHECK(fixture.dashboard.input("\x1b").empty());
    fixture.escapeTimesOut();
    REQUIRE_FALSE(fixture.dashboard.launcher().has_value());
    // `+ new agent` is the only row, selected.
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(fixture.dashboard.launcher().has_value());

    // Unconfigured, `n` says what is missing.
    auto unconfigured = Fixture { World { .configJson = "{}" } };
    CHECK(unconfigured.dashboard.input("n").empty());
    CHECK_FALSE(unconfigured.dashboard.launcher().has_value());
    CHECK(unconfigured.screen().contains("not configured"));
}

TEST_CASE("enter on a finished run runs it again through the launcher", "[orchestrator][view]")
{
    auto fixture = Fixture { fleetWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(NewId).empty());
    // Its item is no longer listed: the launcher does not open, and the action bar says why.
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK_FALSE(fixture.dashboard.launcher().has_value());
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
    // An age that moved on builds them again at the next frame that draws.
    fixture.clock.advance(std::chrono::seconds { 2 });
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
    CHECK_FALSE(fixture.dashboard.launcher().has_value());
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
