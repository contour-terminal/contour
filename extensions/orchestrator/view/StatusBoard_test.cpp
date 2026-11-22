// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/Buffer.hpp>
#include <core/tui/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Board.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Permissions.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/StatusBoard.hpp>
#include <orchestrator/view/Theme.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;
using core::tui::Size;

namespace
{

constexpr auto RunningId = "pr-2375-20261113-150847"sv;
constexpr auto FinishedId = "pr-2366-20261113-140211"sv;

constexpr auto Wide = Size { .width = 120, .height = 40 };
constexpr auto Medium = Size { .width = 80, .height = 24 };
constexpr auto Narrow = Size { .width = 50, .height = 20 };

constexpr auto MinuteMs = uint64_t { 60'000 };

/// A board as an agent writes one: headings, nested lists, a table, a long line, 30 lines in all.
constexpr auto MarkdownBoard = R"(# Status: fix the CI of #2375

## Done
- Reproduced the failing `vtbackend_test` case locally.
- Found the cause: the grid reflow drops a wrapped line's last cell when the
  page is exactly as wide as the line.
)"
                               "\t- Wrote a regression test that fails before the fix.\n"
                               "\t- Fixed `Grid::reflow` to keep the cell.\n"
                               R"(
## In progress
- Running the whole suite under ASan; it takes a while on this machine, so expect this line to stay for a few minutes before it moves.

## Next
1. Push the branch.
2. Watch CI.
3. Ask for a review.

## Notes
| check            | state   |
|------------------|---------|
| ubuntu clang     | passing |
| ubuntu gcc       | running |
| windows          | queued  |

Nothing blocks me right now.



Line 27
Line 28
Line 29
Line 30
)"sv;

/// A world of one running agent and one finished run, its worktree gone.
[[nodiscard]] World boardWorld()
{
    return World { .prs = { "github/prs-empty.json" },
                   .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                               storedRun(Finished { .ended = Ended::Completed,
                                                    .fate = Fate::Removed,
                                                    .entries = {},
                                                    .seen = Seen::Opened,
                                                    .endedWallMs = DispatchWallMs },
                                         FinishedId,
                                         ChangeRequestRef { 2366 }) } };
}

/// A kept finished run, its worktree still there.
constexpr auto KeptId = "pr-2360-20261113-130000"sv;

/// boardWorld(), and a finished run whose worktree was kept.
[[nodiscard]] World keptBoardWorld()
{
    auto world = boardWorld();
    world.stored.push_back(storedRun(Finished { .ended = Ended::Completed,
                                                .fate = Fate::Kept,
                                                .entries = { "M a.txt" },
                                                .seen = Seen::Opened,
                                                .endedWallMs = DispatchWallMs },
                                     KeptId,
                                     ChangeRequestRef { 2360 }));
    return world;
}

/// @return Now on the fixture's clock, in the engine's monotonic milliseconds.
[[nodiscard]] uint64_t monoOf(DashboardFixture const& fixture)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(fixture.clock.now().time_since_epoch())
            .count());
}

/// @return The wall time that goes with @p monoMs: the engine started at DispatchWallMs.
[[nodiscard]] uint64_t wallOf(uint64_t monoMs)
{
    return DispatchWallMs + (monoMs - StartMonoMs);
}

/// The kept run's worktree is checked again and found gone, as when the user removed it by hand: the run's
/// worktree counts as removed.
void removeKeptWorktree(DashboardFixture& fixture)
{
    auto const now = monoOf(fixture);
    for (auto const& effect:
         fixture.engine.step(RetryRemovalOf { .run = std::string { KeptId }, .monoMs = now }))
        if (auto const* const read = std::get_if<ReadFile>(&effect))
            static_cast<void>(
                fixture.engine.step(FileRead { .tag = read->tag,
                                               .content = std::unexpected { FileReadError::DirectoryMissing },
                                               .modifiedWallMs = 0,
                                               .monoMs = now,
                                               .wallMs = wallOf(now) }));
    fixture.dashboard.engineChanged();
    auto const runs = fixture.engine.runs();
    auto const run = std::ranges::find(runs, KeptId, &Run::id);
    REQUIRE(run != runs.end());
    REQUIRE_FALSE(keepsWorktree(*run));
}

/// What the guest does with a ShowBoardAction or a HideBoardAction, and with the read that follows.
/// @param fixture The dashboard and its engine.
/// @param action What the dashboard asked.
/// @param content What the read answers; std::nullopt: it is left unanswered.
/// @param modifiedWallMs The file's modification time.
void carryOut(DashboardFixture& fixture,
              UiAction const& action,
              std::optional<std::expected<std::string, FileReadError>> const& content,
              uint64_t modifiedWallMs = 0)
{
    auto const now = monoOf(fixture);
    auto effects = std::vector<Effect> {};
    if (auto const* const show = std::get_if<ShowBoardAction>(&action))
        effects = fixture.engine.step(
            ShowBoard { .run = show->run, .tab = std::string { DashboardFixture::Tab }, .monoMs = now });
    else if (std::holds_alternative<HideBoardAction>(action))
        effects =
            fixture.engine.step(HideBoard { .tab = std::string { DashboardFixture::Tab }, .monoMs = now });
    for (auto const& effect: effects)
        if (auto const* const read = std::get_if<ReadFile>(&effect); read != nullptr && content)
            static_cast<void>(fixture.engine.step(FileRead { .tag = read->tag,
                                                             .content = *content,
                                                             .modifiedWallMs = modifiedWallMs,
                                                             .monoMs = now,
                                                             .wallMs = wallOf(now) }));
    fixture.dashboard.engineChanged();
}

/// Selects @p run and shows its Board, as the user does: its board is read, as @p content says, and the
/// Focus has the keys.
void openBoard(DashboardFixture& fixture,
               std::string_view run,
               std::optional<std::expected<std::string, FileReadError>> const& content,
               uint64_t modifiedWallMs = 0)
{
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(run).empty());
    REQUIRE(fixture.dashboard.selectedRun() == run);
    for (auto const& action: fixture.dashboard.dueActions())
        carryOut(fixture, action, content, modifiedWallMs);
    CHECK(fixture.dashboard.input("b\t").empty());
}

/// Whether drawnBoard() shows a board from its top or scrolled to its end.
enum class BoardScroll : uint8_t
{
    Top = 0, ///< As opened.
    ToEnd,   ///< At its end.
};

/// Draws the running agent's board, as @p bytes, as the Board tab of a Focus of @p size draws it.
/// @return Its contents, a row each, below the age: the padding and the trailing blanks of each row taken
/// off.
[[nodiscard]] std::vector<std::string> drawnBoard(std::string const& bytes,
                                                  Size size,
                                                  BoardScroll scroll = BoardScroll::Top)
{
    auto fixture = DashboardFixture { boardWorld(), size };
    carryOut(fixture, ShowBoardAction { .run = std::string { RunningId } }, bytes);
    auto subject = FocusSubject { .run = std::string { RunningId } };
    auto state = BoardPanelState {};
    auto panel = StatusBoardPanel { fixture.engine, subject, fixture.clock, state };
    // Inside the Focus's border.
    auto const area = Size { .width = size.width - 2, .height = size.height - 2 };
    auto const draw = [&] {
        auto buffer = core::tui::Buffer { area.height, area.width };
        auto const theme = dashboardTheme();
        auto canvas = core::tui::Canvas {
            buffer, core::tui::Rect { .x = 0, .y = 0, .width = area.width, .height = area.height }, theme
        };
        panel.render(canvas);
        return core::tui::test::canvasToString(buffer);
    };
    auto screen = draw();
    if (scroll == BoardScroll::ToEnd)
    {
        // Scrolled as far as the keys scroll it: wrapped whole, then its last line at the bottom.
        subject.scroll = MaxBoardLines * 4;
        screen = draw();
        subject.scroll = state.lines > state.rows ? state.lines - state.rows : 0;
        screen = draw();
    }
    auto rows = std::vector<std::string> {};
    for (auto const row: std::views::split(screen, '\n'))
    {
        auto text = std::string_view { row };
        if (text.starts_with(" "))
            text.remove_prefix(1);
        rows.emplace_back(text.substr(0, text.find_last_not_of(' ') + 1));
    }
    // The first row is the board's age.
    if (!rows.empty())
        rows.erase(rows.begin());
    return rows;
}

/// U+202E, the right-to-left override, spelled as bytes: misc-misleading-bidirectional rejects it in a
/// literal.
constexpr auto RightToLeftOverrideBytes = std::array { '\xE2', '\x80', '\xAE' };
constexpr auto RightToLeftOverride =
    std::string_view { RightToLeftOverrideBytes.data(), RightToLeftOverrideBytes.size() };

/// @return Whether @p screen shows a character of @p bytes.
[[nodiscard]] bool showsAnyOf(std::string const& screen, std::string_view bytes)
{
    return screen.find_first_of(bytes) != std::string::npos;
}

} // namespace

// {{{ the pure parts

TEST_CASE("a board's age is said in the largest unit that fits", "[orchestrator][view][board]")
{
    struct Case
    {
        uint64_t ageMs = 0;
        std::string_view text = {};
        uint64_t changesInMs = 0;
    };
    constexpr auto HourMs = 60 * MinuteMs;
    for (auto const& [ageMs, text, changesInMs]: std::array {
             Case { .ageMs = 0, .text = "just now", .changesInMs = MinuteMs },
             Case { .ageMs = MinuteMs - 1, .text = "just now", .changesInMs = 1 },
             Case { .ageMs = MinuteMs, .text = "1 min ago", .changesInMs = MinuteMs },
             Case { .ageMs = (4 * MinuteMs) + 30'000, .text = "4 min ago", .changesInMs = 30'000 },
             Case { .ageMs = HourMs - 1, .text = "59 min ago", .changesInMs = 1 },
             Case { .ageMs = HourMs, .text = "1 h ago", .changesInMs = HourMs },
             Case { .ageMs = (48 * HourMs) - 1, .text = "47 h ago", .changesInMs = 1 },
             Case { .ageMs = 48 * HourMs, .text = "2 days ago", .changesInMs = 24 * HourMs },
         })
    {
        CAPTURE(ageMs);
        auto const age = boardAgeOf(ageMs);
        CHECK(age.text == text);
        CHECK(age.changesInMs == changesInMs);
    }
}

TEST_CASE("a 60 KiB line is cut, not wrapped past MaxBoardLines", "[orchestrator][view][board]")
{
    SECTION("one long line is cut to MaxBoardLineChars first: a handful of lines, the last saying so")
    {
        auto const rows = drawnBoard(std::string(std::size_t { 60 } * 1024, 'x'), Medium);
        auto const drawn = (MaxBoardLineChars + 75) / 76;
        REQUIRE(rows.size() > drawn);
        CHECK(rows[drawn - 1].ends_with("…"));
        CHECK(rows[drawn - 2] == std::string(76, 'x'));
        CHECK(rows[drawn].empty());
    }
    SECTION("however the lines wrap, never more than the cap are drawn, and the last says more followed")
    {
        auto many = std::string {};
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, MaxBoardLines))
            many += std::string(MaxBoardLineChars, 'y') + "\n";
        auto const rows = drawnBoard(many, Medium, BoardScroll::ToEnd);
        REQUIRE(rows.size() >= 2);
        CHECK(rows.back() == "…");
        CHECK(rows[rows.size() - 2].starts_with("yyyy"));
    }
    SECTION("a board longer than the cap shows its first lines, then says it was cut")
    {
        auto longBoard = std::string {};
        for (auto const index: std::views::iota(std::size_t { 0 }, MaxBoardLines + 5))
            longBoard += std::format("line {}\n", index);
        CHECK(drawnBoard(longBoard, Medium).front() == "line 0");
        auto const end = drawnBoard(longBoard, Medium, BoardScroll::ToEnd);
        REQUIRE(end.size() >= 2);
        CHECK(end.back() == "…");
        CHECK(end[end.size() - 2] == std::format("line {}", MaxBoardLines - 2));
    }
}

TEST_CASE("a board's wrapped lines keep their indentation", "[orchestrator][view][board]")
{
    // Tabs 16 and 24 cells wide leave the board 12 and 20 cells inside its frame.
    auto const twelve = drawnBoard("  - one two three four", Size { .width = 16, .height = 10 });
    REQUIRE(twelve.size() >= 3);
    CHECK(std::vector(twelve.begin(), twelve.begin() + 3)
          == std::vector<std::string> { "  • one two", "  three four", "" });
    auto const twenty = drawnBoard("\t- x\n\nend", Size { .width = 24, .height = 10 });
    REQUIRE(twenty.size() >= 3);
    CHECK(std::vector(twenty.begin(), twenty.begin() + 3)
          == std::vector<std::string> { "    • x", "", "end" });
}

// }}}
// {{{ golden screens

TEST_CASE("board golden screens", "[orchestrator][view][board]")
{
    struct Scene
    {
        std::string_view name = {};
        std::optional<std::expected<std::string, FileReadError>> content = std::nullopt;
        std::vector<Size> sizes = {};
    };
    auto const scenes = std::array {
        Scene {
            .name = "board", .content = std::string { MarkdownBoard }, .sizes = { Wide, Medium, Narrow } },
        Scene { .name = "board-unread", .content = std::nullopt, .sizes = { Medium } },
        Scene { .name = "board-absent",
                .content = std::unexpected { FileReadError::NotRegular },
                .sizes = { Medium } },
        Scene { .name = "board-gone",
                .content = std::unexpected { FileReadError::DirectoryMissing },
                .sizes = { Medium } },
        Scene { .name = "board-too-large",
                .content = std::unexpected { FileReadError::TooLarge },
                .sizes = { Medium } },
    };
    for (auto const& scene: scenes)
        for (auto const size: scene.sizes)
        {
            auto const name = std::format("{}-{}x{}", scene.name, size.width, size.height);
            SECTION(name)
            {
                auto fixture = DashboardFixture { boardWorld(), size };
                // Written four minutes before it was read.
                openBoard(fixture, RunningId, scene.content, wallOf(monoOf(fixture)) - (4 * MinuteMs));
                checkGolden(name, fixture.screen());
            }
        }
}

TEST_CASE("b is offered on a finished run only while its worktree is there", "[orchestrator][view][board]")
{
    auto fixture = DashboardFixture { keptBoardWorld(), Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(FinishedId).empty());
    CHECK_FALSE(fixture.screen().contains("b board"));
    CHECK(fixture.dashboard.input("b").empty());
    CHECK_FALSE(fixture.dashboard.interaction().chosenTab.has_value());

    auto kept = DashboardFixture { keptBoardWorld(), Medium };
    static_cast<void>(kept.dashboard.frame());
    CHECK(kept.dashboard.select(KeptId).empty());
    CHECK(kept.screen().contains("b board"));
    CHECK(kept.dashboard.input("b").empty());
    REQUIRE(kept.dashboard.interaction().chosenTab.has_value());
    CHECK(kept.dashboard.interaction().chosenTab->tab == FocusTab::Board);
    // The Focus reads the board of the run it shows.
    CHECK(std::ranges::contains(kept.dashboard.dueActions(),
                                UiAction { ShowBoardAction { .run = std::string { KeptId } } }));
}

TEST_CASE("a kept run's board is read once and says plainly when there is none",
          "[orchestrator][view][board]")
{
    auto fixture = DashboardFixture { keptBoardWorld(), Medium };
    openBoard(fixture, KeptId, std::unexpected { FileReadError::NotRegular });
    // Never the prompt's fault: the agent has ended and writes no more.
    checkGolden("board-finished-absent-80x24", fixture.screen());
    // Read once: no later wake reads it again.
    auto const now = monoOf(fixture);
    auto const effects = fixture.engine.step(Wake {
        .monoMs = now + (10 * static_cast<uint64_t>(BoardRefreshEvery.count())), .wallMs = wallOf(now) });
    CHECK(std::ranges::none_of(
        effects, [](Effect const& effect) { return std::holds_alternative<ReadFile>(effect); }));
}

TEST_CASE("a board shown for a run whose worktree was removed says so", "[orchestrator][view][board]")
{
    auto fixture = DashboardFixture { keptBoardWorld(), Medium };
    openBoard(fixture, KeptId, "# kept"s);
    REQUIRE(fixture.screen().contains("kept"));
    removeKeptWorktree(fixture);
    // Its Board tab gives way to Details; the board itself says why, where it is still drawn.
    CHECK(boardScreenText(fixture.engine, KeptId)
          == std::optional<std::string_view> { "The board was removed when the run ended." });
    CHECK_FALSE(fixture.screen().contains("STATUS.md yet"));
}

TEST_CASE("a board under a refused configuration says so", "[orchestrator][view][board]")
{
    auto world = boardWorld();
    world.configJson = "{}";
    auto fixture = DashboardFixture { world, Medium };
    openBoard(fixture, RunningId, "# hi"s);
    // The engine reads nothing without a configuration.
    CHECK(fixture.engine.board(RunningId) == nullptr);
    checkGolden("board-unconfigured-80x24", fixture.screen());
}

TEST_CASE("the Focus reads the board of the run it shows, once", "[orchestrator][view][board]")
{
    // Started with the default profile, which its details name on a line of their own.
    auto world = boardWorld();
    auto const profiles = resolveProfiles({});
    REQUIRE(profiles.has_value());
    world.stored.front().permissions = std::string { DefaultProfileName };
    world.stored.front().settings = settingsJsonOf(*findProfile(*profiles, DefaultProfileName));
    auto fixture = DashboardFixture { std::move(world), Wide };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    auto actions = fixture.dashboard.dueActions();
    std::erase_if(
        actions, [](UiAction const& action) { return std::holds_alternative<ReadTranscriptAction>(action); });
    REQUIRE(actions == std::vector<UiAction> { ShowBoardAction { .run = std::string { RunningId } } });
    carryOut(fixture, actions.front(), "# Fixing CI\n- [x] reproduced\n- [ ] fix the reflow"s);
    auto const screen = fixture.screen();
    CHECK(screen.contains("Fixing CI"));
    CHECK(screen.contains("☑ reproduced"));
    CHECK(screen.contains("☐ fix the reflow"));
    // The row says what the agent is on, and how far it got.
    CHECK(screen.contains("fix the reflow"));
    CHECK(screen.contains("▰▰▰▱▱▱"));
    // Asked for once: nothing more is due while the selection stays.
    CHECK(std::ranges::none_of(fixture.dashboard.dueActions(), [](UiAction const& action) {
        return std::holds_alternative<ShowBoardAction>(action);
    }));

    // A row with no board leaves it unread.
    CHECK(fixture.dashboard.select(FinishedId).empty());
    auto const after = fixture.dashboard.dueActions();
    CHECK(std::ranges::contains(after, UiAction { HideBoardAction {} }));
}

TEST_CASE("b is inert on an orphan row", "[orchestrator][view][board]")
{
    auto fixture = DashboardFixture { World {
        .prs = { "github/prs-empty.json" },
        .worktrees = { WorktreeEntry { .path = "/home/u/worktrees/contour/pr-2-20260101-000000",
                                       .branch = std::nullopt } } } };
    static_cast<void>(fixture.dashboard.frame());
    REQUIRE(fixture.dashboard.selectedRun().empty());
    CHECK(fixture.dashboard.input("b").empty());
    CHECK_FALSE(fixture.dashboard.interaction().chosenTab.has_value());
}

TEST_CASE("the board scrolls by screen, and by line where no message box has the keys",
          "[orchestrator][view][board]")
{
    auto board = std::string {};
    for (auto const index: std::views::iota(1, 101))
        board += std::format("line {}\n", index);
    auto fixture = DashboardFixture { boardWorld(), Wide };
    openBoard(fixture, RunningId, board);
    CHECK(fixture.screen().contains("│ line 1 "));
    auto const& interaction = fixture.dashboard.interaction();
    // The running agent's message box has the keys: the page keys scroll, letters type.
    CHECK(fixture.dashboard.input("\x1b[6~").empty());
    auto const page = interaction.scroll;
    CHECK(page > 10);
    CHECK_FALSE(fixture.screen().contains("│ line 1 "));
    CHECK(fixture.dashboard.input("\x1b[5~").empty());
    CHECK(interaction.scroll == 0);
    CHECK(fixture.dashboard.input("\x1b[F").empty());
    static_cast<void>(fixture.screen());
    CHECK(fixture.screen().contains("│ line 100 "));
    // Past the end it stays at the end.
    auto const last = interaction.scroll;
    CHECK(fixture.dashboard.input("\x1b[6~").empty());
    CHECK(interaction.scroll == last);

    // A kept run has no message box: ↑↓ scroll its Focus.
    auto kept = DashboardFixture { keptBoardWorld(), Wide };
    openBoard(kept, KeptId, board);
    CHECK(kept.dashboard.interaction().place == Place::Focus);
    static_cast<void>(kept.screen());
    CHECK(kept.dashboard.input("j").empty());
    CHECK(kept.dashboard.interaction().scroll == 1);
    CHECK_FALSE(kept.screen().contains("│ line 1 "));
    CHECK(kept.dashboard.input("\x1b[A").empty());
    CHECK(kept.dashboard.interaction().scroll == 0);
}

// }}}
// {{{ what is drawn

TEST_CASE("a board's escapes are drawn inert", "[orchestrator][view][board]")
{
    auto const hostile = std::format("# ok\n\x1b]52;c;aGk=\x07"
                                     "copied\n\x1b[2Jcleared\nabc{}def\n\x9b"
                                     "31mred",
                                     RightToLeftOverride);
    auto fixture = DashboardFixture { boardWorld(), Wide };
    openBoard(fixture, RunningId, hostile);
    auto const screen = fixture.screen();
    CHECK(screen.contains("copied"));
    CHECK(screen.contains("cleared"));
    CHECK(screen.contains("abcdef"));
    CHECK_FALSE(showsAnyOf(screen, "\x1b\x07"));
    CHECK_FALSE(screen.contains(RightToLeftOverride));
    CHECK_FALSE(screen.contains("\xC2\x9B"));
    // Kept raw in the engine: only the drawing sanitises.
    CHECK(fixture.engine.board(RunningId)->bytes == hostile);
}

TEST_CASE("a board's URL is a hyperlink", "[orchestrator][view][board]")
{
    auto fixture = DashboardFixture { boardWorld(), Wide };
    openBoard(fixture, RunningId, "see https://example.com/ci/42 for the log"s);
    static_cast<void>(fixture.screen());
    CHECK(std::ranges::any_of(fixture.dashboard.rendered().hyperlinks(),
                              [](core::tui::HyperlinkRegion const& link) {
                                  return link.uri == "https://example.com/ci/42" && link.cellArea.width == 25;
                              }));
}

TEST_CASE("the board's age follows the clock", "[orchestrator][view][board]")
{
    auto fixture = DashboardFixture { boardWorld(), Wide };
    auto const readAt = monoOf(fixture);
    openBoard(fixture, RunningId, "# hi"s, wallOf(readAt));
    CHECK(fixture.screen().contains("updated just now"));
    auto const at = [](uint64_t monoMs) {
        return core::SteadyTimePoint { std::chrono::milliseconds { monoMs } };
    };
    // The transcript the Focus follows is read again within it: the board's age is due first only once that
    // stops.
    auto const due = fixture.dashboard.nextRepaintDue();
    REQUIRE(due.has_value());
    CHECK(*due <= at(readAt + MinuteMs));

    fixture.clock.advance(std::chrono::seconds { 61 });
    CHECK_FALSE(fixture.dashboard.frame().empty());
    CHECK(core::tui::test::canvasToString(fixture.dashboard.rendered()).contains("updated 1 min ago"));
}

// }}}

TEST_CASE("a paste into the message box keeps its line feeds", "[orchestrator][view][message]")
{
    auto fixture = DashboardFixture { boardWorld(), Wide };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    CHECK(fixture.dashboard.input("\r\x1b[200~one\r\ntwo\x1b[201~").empty());
    CHECK(fixture.dashboard.interaction().drafts.at(std::string { RunningId }) == "one\ntwo");
    // A finished run has no message box: Enter runs it again instead.
    CHECK(fixture.dashboard.input("\x1b").empty());
    fixture.escapeTimesOut();
    CHECK(fixture.dashboard.select(FinishedId).empty());
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK(fixture.screen().contains("#2366 is not listed any more"));
}
