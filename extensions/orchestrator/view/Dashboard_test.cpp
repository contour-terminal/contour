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

/// The engine's monotonic time six minutes after it started, as the guest's clock would read it.
[[nodiscard]] core::SteadyTimePoint sixMinutesIn()
{
    return core::SteadyTimePoint { std::chrono::milliseconds { StartMonoMs + uint64_t { 360'000 } } };
}

/// The dashboard over an engine, with the clock it reads.
struct Fixture
{
    explicit Fixture(World const& world, Size size = Wide):
        engine { bootEngine(world) },
        clock { sixMinutesIn() },
        dashboard { engine, dashboardTheme(), clock, size }
    {
    }

    /// Lets a held ESC wait past its deadline, and draws: it is then the Escape key.
    void escapeTimesOut()
    {
        clock.advance(std::chrono::milliseconds { 60 });
        static_cast<void>(dashboard.frame());
    }

    /// Draws a frame and returns the screen as text.
    [[nodiscard]] std::string screen()
    {
        static_cast<void>(dashboard.frame());
        return core::tui::test::canvasToString(dashboard.rendered());
    }

    Engine engine;
    core::ManualClock clock;
    Dashboard dashboard;
};

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

/// The worlds the golden screens are drawn from.
struct Scene
{
    std::string_view name = {};
    World world = {};
    std::string_view keys = {}; ///< Typed before the screen is drawn.
};

[[nodiscard]] std::vector<Scene> scenes()
{
    auto const listsOnly = World { .issues = { "github/issues-open.json" } };
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
        for (auto const size: { Wide, Medium, Narrow })
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
        if (!row.hint.empty())
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
        { .name = "enter in Narrow",
          .world = &lists,
          .size = Narrow,
          .keys = "\r",
          .expected = { .pane = Pane::Details } },
        { .name = "enter, esc in Narrow", .world = &lists, .size = Narrow, .keys = "\r\x1b", .expected = {} },
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
    // Row 0 is the frame, row 1 the first heading; the first pull request is on row 2.
    static_cast<void>(fixture.screen());
    auto const isSelected = [&](int row) {
        return core::tui::test::styleAt(fixture.dashboard.rendered(), row, 3).inverse;
    };
    CHECK(isSelected(2));
    CHECK(!isSelected(3));
    CHECK(fixture.dashboard.input("j").empty());
    static_cast<void>(fixture.screen());
    CHECK(!isSelected(2));
    CHECK(isSelected(3));
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

    fixture.dashboard.select(BlockedId);
    CHECK(fixture.dashboard.selectedRun() == BlockedId);
    CHECK(!fixture.dashboard.frame().empty());

    // A filter that hides the row is dropped; an unknown run selects nothing else.
    CHECK(fixture.dashboard.input("/2375\r").empty());
    REQUIRE(fixture.dashboard.interaction().filter == "2375");
    fixture.dashboard.select(BlockedId);
    CHECK(fixture.dashboard.selectedRun() == BlockedId);
    CHECK(fixture.dashboard.interaction().filter.empty());
    fixture.dashboard.select("no-such-run");
    CHECK(fixture.dashboard.selectedRun() == BlockedId);

    // An end that waits for its confirmation is dropped: it was asked of another row.
    fixture.dashboard.select(RunningId);
    CHECK(fixture.dashboard.input("e").empty());
    REQUIRE(fixture.dashboard.interaction().pendingEnd == RunningId);
    fixture.dashboard.select(BlockedId);
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
    auto dashboard = Dashboard { engine, dashboardTheme(), clock, Wide };
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
