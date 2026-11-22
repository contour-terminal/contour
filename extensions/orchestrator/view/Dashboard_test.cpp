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
#include <orchestrator/view/Dashboard.hpp>
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
constexpr auto SeenId = "issue-2338-20261112-093000"sv;
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

/// A run that was active from @p hoursAgo to @p untilHoursAgo before the fixture's now, spending @p tokens.
[[nodiscard]] Run pastRun(
    std::string_view id, uint64_t number, double hoursAgo, double untilHoursAgo, uint64_t tokens)
{
    constexpr auto Hour = 3'600'000.0;
    auto const from = DispatchWallMs - static_cast<uint64_t>(hoursAgo * Hour);
    auto const to = DispatchWallMs - static_cast<uint64_t>(untilHoursAgo * Hour);
    auto run = finishedRun(id, ChangeRequestRef { number }, Fate::Removed, Seen::Opened, "fix_ci");
    run.startedWallMs = from;
    std::get<Finished>(run.state).endedWallMs = to;
    run.history = { StateChange { .wallMs = from, .state = "preparing" },
                    StateChange { .wallMs = from, .state = "running" },
                    StateChange { .wallMs = to, .state = "ending" },
                    StateChange { .wallMs = to, .state = "finished" } };
    run.stats = RunStats {};
    run.stats->tokens.output = tokens;
    return run;
}

/// A day of runs: a quiet night, a busy morning, and one still running.
[[nodiscard]] std::vector<Run> dayOfRuns()
{
    return {
        pastRun("pr-2001-20261112-160000", 2001, 22.5, 21.0, 30'000),
        pastRun("pr-2002-20261112-230000", 2002, 9.0, 7.5, 60'000),
        pastRun("pr-2003-20261113-000000", 2003, 8.5, 6.0, 120'000),
        pastRun("pr-2004-20261113-010000", 2004, 7.5, 7.0, 15'000),
        pastRun("pr-2005-20261113-120000", 2005, 3.0, 1.0, 90'000),
        storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
    };
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

/// The worlds the golden screens are drawn from.
struct Scene
{
    std::string_view name = {};
    World world = {};
    std::string_view keys = {};                         ///< Typed before the screen is drawn.
    std::vector<Size> sizes = { Wide, Medium, Narrow }; ///< The sizes it is drawn at.
};

[[nodiscard]] std::vector<Scene> scenes()
{
    auto const listsOnly = World { .issues = { "github/issues-open.json" } };
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
    return {
        Scene { .name = "empty", .world = World { .configJson = "{}" } },
        Scene { .name = "lists", .world = listsOnly },
        Scene { .name = "problem", .world = World { .prs = { "" } } },
        Scene { .name = "incomplete", .world = World { .prs = { "github/prs-100.json" } } },
        Scene { .name = "selected", .world = listsOnly, .keys = "jj" },
        Scene { .name = "done-run",
                .world = World { .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                                             storedRun(Done {}, DoneId, ChangeRequestRef { 2379 }) } },
                .keys = "\x1b[F"sv },
        Scene { .name = "confirm-end",
                .world = World { .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }) } },
                .keys = "\x1b[Fe"sv },
        Scene { .name = "finished",
                .world = World { .stored = {
                                     finishedRun(NewId, ChangeRequestRef { 2366 }, Fate::Removed, Seen::New, "fix_ci"),
                                     finishedRun(SeenId, IssueRef { 2338 }, Fate::Removed, Seen::Opened, "issue"),
                                     finishedRun(KeptId, IssueRef { 2356 }, Fate::Kept, Seen::Opened, "issue"),
                                 } },
                .keys = "\x1b[F"sv },
        Scene { .name = "outside",
                .world = World { .prs = { "github/prs-empty.json" },
                                 .stored = {
                                     outsideConfiguration(storedRun(Running {}, RunningId, ChangeRequestRef { 2375 })),
                                     outsideConfiguration(
                                         finishedRun(KeptId, IssueRef { 2356 }, Fate::Kept, Seen::Opened, "issue")),
                                 } },
                .keys = "\x1b[F"sv,
                .sizes = { Wide, Narrow } },
        Scene { .name = "trust-hint",
                .world = World { .prs = { "github/prs-empty.json" }, .stored = { unreportedRun() } },
                .keys = "\x1b[F"sv,
                .sizes = { Medium } },
        Scene { .name = "kept-reasons",
                .world = keptReasons,
                .keys = "\x1b[F"sv,
                .sizes = { Wide } },
        Scene { .name = "kept-reasons",
                .world = keptReasons,
                .keys = "\x1b[F\t"sv,
                .sizes = { Narrow } },
        // The timeline charts a day of runs, an hour a bar, in the Wide band only.
        Scene { .name = "timeline",
                .world = World { .prs = { "github/prs-empty.json" }, .stored = dayOfRuns() },
                .keys = {},
                .sizes = { Wide, Medium } },
        // A finished run's details carry what its agent spent and did, and how long it waited for the user.
        Scene { .name = "finished-stats", .world = withStats, .keys = "\x1b[F"sv, .sizes = { Wide } },
        Scene { .name = "finished-stats", .world = withStats, .keys = "\x1b[F\t"sv, .sizes = { Narrow } },
        Scene { .name = "pane-refused", .world = paneRefused, .keys = "\x1b[F"sv, .sizes = { Wide } },
        Scene { .name = "pane-refused", .world = paneRefused, .keys = "\x1b[F\t"sv, .sizes = { Narrow } },
        Scene { .name = "orphans", .world = orphansWorld(), .keys = "\x1b[F"sv, .sizes = { Medium } },
        // Enter opens an orphan's details on the whole width, for its command line.
        Scene { .name = "orphans", .world = orphansWorld(), .keys = "\x1b[F\r"sv, .sizes = { Wide, Narrow } },
    };
}

/// The PR list with the first title replaced.
[[nodiscard]] std::string prsTitled(std::string_view jsonTitle)
{
    auto prs = readFixture(FixtureRoot::Testdata, "github/prs-open.json");
    constexpr auto Original =
        "Say why the stdio relay failed, and send a hello before half-closing in the starter case"sv;
    auto const at = prs.find(Original);
    REQUIRE(at != std::string::npos);
    prs.replace(at, Original.size(), jsonTitle);
    return prs;
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
            core::tui::Breakpoint { .minColumns = row.minColumns, .minRows = row.minRows, .make = {} });
    struct Case
    {
        Size size = {};
        std::size_t index = 0;
    };
    auto const cases = std::array {
        Case { .size = { .width = 59, .height = 19 }, .index = 0 },
        Case { .size = { .width = 59, .height = 20 }, .index = 1 },
        Case { .size = { .width = 60, .height = 19 }, .index = 2 },
        Case { .size = { .width = 99, .height = 20 }, .index = 3 },
        Case { .size = { .width = 100, .height = 19 }, .index = 4 },
        Case { .size = { .width = 100, .height = 20 }, .index = 5 },
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
                CHECK(fixture.dashboard.input(scene.keys).empty());
                checkGolden(name, fixture.screen());
            }
        }
}

TEST_CASE("a resize from 100x40 to 100x18 collapses the hints and headings", "[orchestrator][view]")
{
    auto fixture = Fixture { World {}, { .width = 100, .height = 40 } };
    auto const tall = fixture.screen();
    CHECK(fixture.dashboard.breakpoint() == std::optional<std::size_t> { 5 });
    CHECK(tall.contains("MY PULL REQUESTS"));
    CHECK(tall.contains("enter open"));

    fixture.dashboard.resize({ .width = 100, .height = 18 });
    auto const low = fixture.screen();
    CHECK(fixture.dashboard.breakpoint() == std::optional<std::size_t> { 4 });
    CHECK(!low.contains("MY PULL REQUESTS"));
    CHECK(!low.contains("enter open"));
    checkGolden("lists-collapsed-100x18", low);
}

TEST_CASE("a resize repaints at the new band", "[orchestrator][view]")
{
    auto fixture = Fixture { World {}, Wide };
    CHECK(fixture.screen().contains("Details"));
    CHECK(fixture.dashboard.breakpoint() == std::optional<std::size_t> { 5 });
    fixture.dashboard.resize(Narrow);
    auto const bytes = fixture.dashboard.frame();
    CHECK(!bytes.empty());
    CHECK(fixture.dashboard.breakpoint() == std::optional<std::size_t> { 1 });
    auto const narrow = core::tui::test::canvasToString(fixture.dashboard.rendered());
    CHECK(!narrow.contains("Details"));
    CHECK(fixture.dashboard.rendered().cols() == Narrow.width);
}

TEST_CASE("the dashboard shows no cursor, and rests it at the top-left", "[orchestrator][view]")
{
    constexpr auto ShowCursor = "\033[?25h"sv;
    for (auto const size: { Wide, Medium, Narrow })
    {
        auto fixture = Fixture { World {}, size };
        // Each frame draws into the other of the screen's two buffers, so several are looked at, among them
        // the frames that open the filter and the help.
        for (auto const keys: { ""sv, "j"sv, "/"sv, "\033"sv, "?"sv })
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
// {{{ what is drawn

TEST_CASE("a title with an escape sequence is inert", "[orchestrator][view]")
{
    auto fixture =
        Fixture { World { .prs = { .fixture = "", .text = prsTitled(R"(\u001b]52;c;ZXZpbA==\u0007pwn)") } } };
    auto const bytes = fixture.dashboard.frame();
    CHECK(!bytes.empty());
    CHECK(!bytes.contains("\x1b]"));
    CHECK(!bytes.contains('\x07'));
    CHECK(core::tui::test::canvasToString(fixture.dashboard.rendered()).contains("#2375 ]52;c;ZXZpbA==pwn"));
}

TEST_CASE("wide characters at the column edge of a row", "[orchestrator][view]")
{
    auto wide = std::string {};
    while (columnsOf(wide) < 120)
        wide += "語";
    for (auto const size: { Wide, Medium, Narrow, Size { .width = 81, .height = 24 } })
    {
        CAPTURE(size.width);
        auto fixture = Fixture { World { .prs = { .fixture = "", .text = prsTitled(wide) } }, size };
        auto const screen = fixture.screen();
        auto lines = std::vector<std::string> {};
        for (auto const& line: screen | std::views::split('\n'))
            lines.emplace_back(line.begin(), line.end());
        auto const row =
            std::ranges::find_if(lines, [](std::string const& line) { return line.contains("#2375 語"); });
        REQUIRE(row != lines.end());
        CHECK(row->contains("語…"));
        CHECK(columnsOf(*row) == size.width);
    }
}

TEST_CASE("the help lists the keys that have a hint", "[orchestrator][view]")
{
    auto fixture = Fixture { World {} };
    CHECK(!fixture.screen().contains(" Keys "));
    CHECK(fixture.dashboard.input("?").empty());
    auto const help = fixture.screen();
    CHECK(help.contains("╭─ Keys "));
    for (auto const& row: keyRows())
    {
        CAPTURE(row.key);
        if (!row.hint.empty() && holds(row.on, Screen::Dashboard))
            CHECK(help.contains(std::format("│ {} ", row.hint)));
    }
    CHECK(fixture.dashboard.input("\x1b").empty());
    fixture.escapeTimesOut();
    CHECK(!fixture.screen().contains(" Keys "));
}

// }}}
// {{{ input

TEST_CASE("key input", "[orchestrator][view]")
{
    auto const lists = World {};
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
    auto const selectableIn = [](World const& world) {
        auto fixture = Fixture { world };
        return static_cast<std::size_t>(
            std::ranges::count_if(buildRows(fixture.engine, StartMonoMs), &Row::isSelectable));
    };
    auto const last = selectableIn(lists) - 1;

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
        { .name = "down", .world = &lists, .size = Wide, .keys = "\x1b[B", .expected = { .selected = 1 } },
        { .name = "j", .world = &lists, .size = Wide, .keys = "j", .expected = { .selected = 1 } },
        { .name = "j j k", .world = &lists, .size = Wide, .keys = "jjk", .expected = { .selected = 1 } },
        { .name = "up at the top",
          .world = &lists,
          .size = Wide,
          .keys = "\x1b[A",
          .expected = { .selected = 0 } },
        { .name = "end", .world = &lists, .size = Wide, .keys = "\x1b[F", .expected = { .selected = last } },
        { .name = "end, down",
          .world = &lists,
          .size = Wide,
          .keys = "\x1b[Fj",
          .expected = { .selected = last } },
        { .name = "home", .world = &lists, .size = Wide, .keys = "jj\x1b[H", .expected = { .selected = 0 } },
        { .name = "filter",
          .world = &lists,
          .size = Wide,
          .keys = "/stdio",
          .expected = { .filtering = Filtering::Typing, .filter = "stdio" } },
        { .name = "filter kept",
          .world = &lists,
          .size = Wide,
          .keys = "/stdio\r",
          .expected = { .filter = "stdio" } },
        { .name = "filter dropped", .world = &lists, .size = Wide, .keys = "/stdio\x1b", .expected = {} },
        { .name = "tab in Medium",
          .world = &lists,
          .size = Medium,
          .keys = "\t",
          .expected = { .pane = Pane::Details } },
        { .name = "tab twice in Medium", .world = &lists, .size = Medium, .keys = "\t\t", .expected = {} },
        { .name = "tab in Wide", .world = &lists, .size = Wide, .keys = "\t", .expected = {} },
        { .name = "tab in Narrow",
          .world = &lists,
          .size = Narrow,
          .keys = "\t",
          .expected = { .pane = Pane::Details } },
        { .name = "tab, esc in Narrow", .world = &lists, .size = Narrow, .keys = "\t\x1b", .expected = {} },
        { .name = "help",
          .world = &lists,
          .size = Wide,
          .keys = "?",
          .expected = { .overlay = Overlay::Help } },
        { .name = "help closed", .world = &lists, .size = Wide, .keys = "?\x1b", .expected = {} },
        { .name = "refresh",
          .world = &lists,
          .size = Wide,
          .keys = "R",
          .expected = {},
          .actions = { RefreshNowAction {} } },
        { .name = "e on a pull request", .world = &lists, .size = Wide, .keys = "e", .expected = {} },
        { .name = "e on an active run asks",
          .world = &withRun,
          .size = Wide,
          .keys = "\x1b[Fe",
          .expected = { .selected = selectableIn(withRun) - 1, .pendingEnd = std::string { RunningId } } },
        { .name = "e, e on an active run",
          .world = &withRun,
          .size = Wide,
          .keys = "\x1b[Fee",
          .expected = { .selected = selectableIn(withRun) - 1 },
          .actions = { EndRunAction { .run = std::string { RunningId } } } },
        { .name = "e, k keeps the run and does not move",
          .world = &withRun,
          .size = Wide,
          .keys = "\x1b[Fek",
          .expected = { .selected = selectableIn(withRun) - 1 } },
        { .name = "e, esc keeps the run",
          .world = &withRun,
          .size = Wide,
          .keys = "\x1b[Fe\x1b",
          .expected = { .selected = selectableIn(withRun) - 1 } },
        { .name = "delete while typing a filter",
          .world = &withKept,
          .size = Wide,
          .keys = "\x1b[F/\x1b[3~",
          .expected = { .selected = selectableIn(withKept) - 1, .filtering = Filtering::Typing } },
        { .name = "x on a kept entry",
          .world = &withKept,
          .size = Wide,
          .keys = "\x1b[Fx",
          .expected = { .selected = selectableIn(withKept) - 1 },
          .actions = { RetryRemovalAction { .run = std::string { KeptId } } } },
        { .name = "enter on a new finished entry",
          .world = &withKept,
          .size = Wide,
          .keys = "\x1b[F\r",
          .expected = { .selected = selectableIn(withKept) - 1 },
          .actions = { MarkSeenAction { .run = std::string { KeptId } } } },
        { .name = "x on a kept entry outside the configuration",
          .world = &outsideKept,
          .size = Wide,
          .keys = "\x1b[Fx",
          .expected = { .selected = selectableIn(outsideKept) - 1 } },
        { .name = "e, e on an active run outside the configuration",
          .world = &outsideRun,
          .size = Wide,
          .keys = "\x1b[Fee",
          .expected = { .selected = selectableIn(outsideRun) - 1 },
          .actions = { EndRunAction { .run = std::string { RunningId } } } },
        { .name = "delete on a finished entry outside the configuration",
          .world = &outsideKept,
          .size = Wide,
          .keys = "\x1b[F\x1b[3~",
          .expected = { .selected = selectableIn(outsideKept) - 1 },
          .actions = { DismissAction { .run = std::string { KeptId } } } },
        { .name = "delete on a finished entry",
          .world = &withKept,
          .size = Wide,
          .keys = "\x1b[F\x1b[3~",
          .expected = { .selected = selectableIn(withKept) - 1 },
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

TEST_CASE("an ESC that ends an input waits for the rest of its sequence", "[orchestrator][view]")
{
    SECTION("ESC, then [B: one Down")
    {
        auto fixture = Fixture { World {} };
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
        auto fixture = Fixture { World {} };
        CHECK(fixture.dashboard.input("?").empty());
        CHECK(fixture.dashboard.input("\x1b").empty());
        CHECK(fixture.dashboard.interaction() == Interaction { .overlay = Overlay::Help });
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
        auto fixture = Fixture { World {} };
        CHECK(fixture.dashboard.input("?").empty());
        CHECK(fixture.dashboard.input("\x1b").empty());
        CHECK(fixture.dashboard.input("j").empty());
        fixture.escapeTimesOut();
        CHECK(fixture.dashboard.interaction() == Interaction { .overlay = Overlay::Help });
    }
    SECTION("ESC, then j after the deadline: Escape, then j")
    {
        auto fixture = Fixture { World {} };
        CHECK(fixture.dashboard.input("?").empty());
        CHECK(fixture.dashboard.input("\x1b").empty());
        fixture.clock.advance(std::chrono::milliseconds { 60 });
        CHECK(fixture.dashboard.input("j").empty());
        CHECK(fixture.dashboard.interaction() == Interaction { .selected = 1 });
    }
}

TEST_CASE("an escape sequence split across two input calls", "[orchestrator][view]")
{
    auto fixture = Fixture { World {} };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.input("\x1b[").empty());
    CHECK(fixture.dashboard.interaction() == Interaction {});
    CHECK(fixture.dashboard.input("B").empty());
    CHECK(fixture.dashboard.interaction() == Interaction { .selected = 1 });
}

// {{{ Enter, d and the task keys

namespace
{

/// A configuration whose `tasks` rows are @p tasks, and otherwise the default world's.
[[nodiscard]] World withTasks(std::string_view tasks)
{
    return World { .configJson =
                       std::format(R"({{ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "repos": {}, "tasks": [ {} ] }})",
                                   OneRepository,
                                   tasks) };
}

constexpr auto AddressReview = R"({ "id": "address_review", "key": "a", "label": "address review",
    "applies_to": "pr", "model": "opus", "prompt": "/contour-workflows:address-review {url}" })"sv;
constexpr auto WorkIssue = R"({ "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue",
    "model": "opus", "prompt": "/contour-workflows:work-issue {url}" })"sv;
constexpr auto FixCi =
    R"({ "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
    "prompt": "/contour-workflows:fix-ci {url}" })"sv;

/// @return The id of the task the open form is on.
[[nodiscard]] std::string formTask(DashboardFixture const& fixture)
{
    auto const& launcher = fixture.dashboard.launcher();
    REQUIRE(launcher.has_value());
    auto const& form = launcher->form;
    REQUIRE(form.has_value());
    auto const* const config = fixture.engine.config();
    REQUIRE(config != nullptr);
    return config->tasks()[form->task].id;
}

} // namespace

TEST_CASE("the help lists the tasks' keys", "[orchestrator][view]")
{
    auto fixture = DashboardFixture { World {} };
    CHECK(fixture.dashboard.input("?").empty());
    auto const screen = fixture.screen();
    CHECK(screen.contains(" tasks "));
    CHECK(screen.contains("f fix CI"));
    CHECK(screen.contains("w work on issue"));
}

TEST_CASE("a hint line is cut between hints, never inside one", "[orchestrator][view]")
{
    static constexpr auto Hints = std::array { "enter open"sv, "d dispatch"sv, "esc back"sv };
    CHECK(joinHints(Hints, 100) == "enter open · d dispatch · esc back");
    CHECK(joinHints(Hints, 30) == "enter open · d dispatch");
    CHECK(joinHints(Hints, 22) == "enter open");
    CHECK(joinHints(Hints, 5).empty());
}

TEST_CASE("Enter's default task follows the tasks rows", "[orchestrator][view]")
{
    SECTION("without fix_ci, a pull request's first task is address_review")
    {
        auto fixture = DashboardFixture { withTasks(std::format("{}, {}", WorkIssue, AddressReview)) };
        CHECK(fixture.dashboard.input("\r").empty());
        CHECK(formTask(fixture) == "address_review");
        CHECK_FALSE(fixture.dashboard.launcher()->form->session.empty());
        CHECK(fixture.screen().contains("New agent"));
    }
    SECTION("with no task for a pull request, Enter on one opens nothing and says so")
    {
        auto fixture = DashboardFixture { withTasks(WorkIssue) };
        CHECK(fixture.dashboard.input("\r").empty());
        CHECK_FALSE(fixture.dashboard.launcher().has_value());
        CHECK(fixture.screen().contains("no task applies"));
        CHECK(fixture.ids.taken == 0);
    }
    SECTION("d opens the same form")
    {
        auto fixture = DashboardFixture { World {} };
        CHECK(fixture.dashboard.input("d").empty());
        CHECK(formTask(fixture) == "fix_ci");
    }
}

TEST_CASE("task keys open the form on that task", "[orchestrator][view]")
{
    auto fixture = DashboardFixture { withTasks(std::format("{}, {}, {}", FixCi, AddressReview, WorkIssue)) };
    CHECK(fixture.dashboard.input("a").empty());
    CHECK(formTask(fixture) == "address_review");
    CHECK(fixture.dashboard.input("\x1b").empty());
    fixture.escapeTimesOut();
    REQUIRE_FALSE(fixture.dashboard.launcher().has_value());

    CHECK(fixture.dashboard.input("f").empty());
    CHECK(formTask(fixture) == "fix_ci");

    // A task for issues does not apply to a pull request: the key says so and opens nothing.
    auto other = DashboardFixture { withTasks(std::format("{}, {}", FixCi, WorkIssue)) };
    CHECK(other.dashboard.input("w").empty());
    CHECK_FALSE(other.dashboard.launcher().has_value());
    CHECK(other.screen().contains("work on issue does not apply to this item"));
}

TEST_CASE("Enter on an item with an active run reveals its agent", "[orchestrator][view]")
{
    auto fixture = DashboardFixture { World {
        .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }) } } };
    // The first row is #2375, whose agent runs.
    auto const actions = fixture.dashboard.input("\r");
    CHECK(actions == std::vector<UiAction> { RevealAgentAction { .run = std::string { RunningId } } });
    CHECK_FALSE(fixture.dashboard.launcher().has_value());

    // And on the agent's own row.
    CHECK(fixture.dashboard.input("\x1b[F\r")
          == std::vector<UiAction> { RevealAgentAction { .run = std::string { RunningId } } });
}

// }}}
// {{{ frames

TEST_CASE("the rows are built once per input, not once per key", "[orchestrator][view]")
{
    auto fixture = Fixture { World {} };
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
    auto fixture = Fixture { World {} };
    // Row 0 is the frame, row 1 the first heading, row 2 its repository's group; the first pull request is on
    // row 3.
    static_cast<void>(fixture.screen());
    auto const isSelected = [&](int row) {
        return core::tui::test::styleAt(fixture.dashboard.rendered(), row, 3).inverse;
    };
    CHECK(!isSelected(2));
    CHECK(isSelected(3));
    CHECK(!isSelected(4));
    CHECK(fixture.dashboard.input("j").empty());
    static_cast<void>(fixture.screen());
    CHECK(!isSelected(3));
    CHECK(isSelected(4));
}

TEST_CASE("a second frame with nothing changed emits nothing", "[orchestrator][view]")
{
    auto fixture = Fixture { World {} };
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
    CHECK(fixture.dashboard.selectedRun().empty());

    CHECK(fixture.dashboard.select(BlockedId).empty());
    CHECK(fixture.dashboard.selectedRun() == BlockedId);
    CHECK(!fixture.dashboard.frame().empty());

    // A filter that hides the row is dropped; an unknown run selects nothing else.
    CHECK(fixture.dashboard.input("/2375\r").empty());
    REQUIRE(fixture.dashboard.interaction().filter == "2375");
    CHECK(fixture.dashboard.select(BlockedId).empty());
    CHECK(fixture.dashboard.selectedRun() == BlockedId);
    CHECK(fixture.dashboard.interaction().filter.empty());
    CHECK(fixture.dashboard.select("no-such-run").empty());
    CHECK(fixture.dashboard.selectedRun() == BlockedId);

    // An end that waits for its confirmation is dropped: it was asked of another row.
    CHECK(fixture.dashboard.select(RunningId).empty());
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
    CHECK(fixture.dashboard.input("\x1b[F").empty());
    REQUIRE(fixture.dashboard.selectedRun() == KeptId);
    CHECK(fixture.dashboard.input("x").empty());
    CHECK(fixture.dashboard.input("xx").empty());
    CHECK_FALSE(fixture.screen().contains("x tries the removal again"));
}

TEST_CASE("the e prompt answered no does nothing", "[orchestrator][view]")
{
    auto fixture =
        Fixture { World { .prs = { "github/prs-empty.json" },
                          .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }) } } };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.input("\x1b[Fe").empty());
    REQUIRE(fixture.dashboard.interaction().pendingEnd == RunningId);
    CHECK(fixture.dashboard.input("n").empty());
    CHECK(fixture.dashboard.interaction().pendingEnd.empty());
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
    CHECK(statusAt(29'000).status == "working 29s");
    CHECK(!std::ranges::contains(statusAt(29'000).details, hint));
    CHECK(statusAt(30'000).status == "waiting? see its tab");
    CHECK(statusAt(30'000).statusRole == Role::Warning);
    CHECK(std::ranges::contains(statusAt(30'000).details, hint));
    // Still endable, and nothing else: the hint answers nothing.
    CHECK(statusAt(30'000).offers
          == std::vector { KeyCommand::Board, KeyCommand::Transcript, KeyCommand::EndRun });

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
    // A hinted row shows no age: nothing ticks under it.
    CHECK(dashboard.nextRepaintDue() == std::nullopt);

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

TEST_CASE("an orphan's row is read-only: Enter and Tab show its details", "[orchestrator][view]")
{
    auto fixture = Fixture { orphansWorld(), Narrow };
    CHECK(fixture.dashboard.input("\x1b[F"sv).empty());
    CHECK(!fixture.screen().contains("to remove git -C"));
    // No key acts on it: nothing is asked, nothing reaches the engine.
    for (auto const key: { "e"sv, "x"sv, "\x1b[3~"sv })
    {
        CAPTURE(key);
        CHECK(fixture.dashboard.input(key).empty());
        CHECK(!fixture.screen().contains("to confirm"));
    }
    CHECK(fixture.dashboard.input("\r"sv).empty());
    CHECK(fixture.screen().contains("to remove git -C"));
    static_cast<void>(fixture.dashboard.input("\x1b"sv));
    fixture.escapeTimesOut();
    CHECK(!fixture.screen().contains("to remove git -C"));
    CHECK(fixture.dashboard.input("\t"sv).empty());
    CHECK(fixture.screen().contains("to remove git -C"));
}

TEST_CASE("Enter on an orphan in the Wide band shows its details on the whole width", "[orchestrator][view]")
{
    constexpr auto Command =
        R"(git -C /home/u/projects/contour worktree remove '/home/u/worktrees/contour/it'\''s a test')"sv;
    auto fixture = Fixture { orphansWorld(), Wide };
    CHECK(fixture.dashboard.input("\x1b[F"sv).empty());
    // Beside the list the panel is too narrow for the line: it is cut, never wrapped.
    CHECK(!fixture.screen().contains(Command));
    CHECK(fixture.dashboard.input("\r"sv).empty());
    auto const page = fixture.screen();
    CHECK(page.contains(Command));
    CHECK(!page.contains("LEFTOVER WORKTREES"));
    static_cast<void>(fixture.dashboard.input("\x1b"sv));
    fixture.escapeTimesOut();
    CHECK(fixture.screen().contains("LEFTOVER WORKTREES"));
}

TEST_CASE("rows that arrive above the selection keep its heading in view", "[orchestrator][view]")
{
    // The worktree list answers before the pull requests: the selection first lands on an orphan, far down,
    // and the list scrolls to it; the pull requests then arrive above and take the selection.
    auto world = orphansWorld();
    world.prs = { "github/prs-open.json" };
    world.held = { "gh-prs", "gh-issues" };
    for (auto const index: std::views::iota(0, 4))
        world.worktrees.push_back(WorktreeEntry {
            .path = std::format("/home/u/worktrees/contour/pr-{}", index), .branch = std::nullopt });
    auto fixture = Fixture { world, Medium };
    REQUIRE(!fixture.held.empty());
    CHECK(!fixture.screen().contains("MY PULL REQUESTS"));
    fixture.answerHeld();
    auto const screen = fixture.screen();
    CHECK(screen.contains("#2375"));
    CHECK(screen.contains("MY PULL REQUESTS"));
}

TEST_CASE("scrolling back up to a section's first item shows its heading", "[orchestrator][view]")
{
    // At 80x24 the list is shorter than its rows: End scrolls the PULL REQUESTS heading away, and Home must
    // bring it back with the first item, not stop at the item.
    auto fixture = Fixture { World { .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                                                 storedRun(Done {}, DoneId, ChangeRequestRef { 2379 }) } },
                             Medium };
    CHECK(fixture.dashboard.input("\x1b[F"sv).empty());
    CHECK(!fixture.screen().contains("MY PULL REQUESTS"));
    CHECK(fixture.dashboard.input("\x1b[H"sv).empty());
    CHECK(fixture.screen().contains("MY PULL REQUESTS"));
}

TEST_CASE("z folds the selected item's group and selects its header; enter unfolds it",
          "[orchestrator][view]")
{
    auto fixture = Fixture { World {} };
    static_cast<void>(fixture.screen());
    REQUIRE(fixture.dashboard.selectedRun().empty());
    auto const toggle = std::vector<UiAction> { ToggleFoldAction { .group = "prs/contour" } };

    CHECK(fixture.dashboard.input("z") == toggle);
    static_cast<void>(fixture.engine.step(ToggleFold { .group = "prs/contour", .monoMs = StartMonoMs }));
    fixture.dashboard.engineChanged();
    auto const folded = fixture.screen();
    CHECK(folded.contains("▸ contour · 5"));
    CHECK_FALSE(folded.contains("#2375"));

    // The header is selected now: Enter unfolds the group, and the selection moves to its first item.
    CHECK(fixture.dashboard.input("\r") == toggle);
    static_cast<void>(fixture.engine.step(ToggleFold { .group = "prs/contour", .monoMs = StartMonoMs }));
    fixture.dashboard.engineChanged();
    auto const open = fixture.screen();
    CHECK(open.contains("▾ contour · 5"));
    CHECK(open.contains("#2375"));
    CHECK(fixture.dashboard.input("z") == toggle);
}

TEST_CASE("a section's heading stays in view above its first group's header", "[orchestrator][view]")
{
    // Scrolled so that the first group's header is the top row, the selection on the item below it: the
    // section's heading comes back into view, above the header.
    auto fixture = Fixture { World {}, Size { .width = 80, .height = 24 } };
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.input(std::string(12, 'j')).empty());
    static_cast<void>(fixture.screen());
    CHECK(fixture.dashboard.input("\x1b[H").empty());
    auto const screen = fixture.screen();
    CHECK(screen.contains("MY PULL REQUESTS"));
    CHECK(screen.contains("▾ contour · 5"));
}
