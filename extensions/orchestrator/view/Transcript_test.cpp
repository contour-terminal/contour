// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Buffer.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/LazyLines.hpp>
#include <orchestrator/view/Theme.hpp>
#include <orchestrator/view/Transcript.hpp>
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
constexpr auto FinishedId = "pr-2369-20261113-140211"sv;

constexpr auto Wide = Size { .width = 120, .height = 40 };
constexpr auto Medium = Size { .width = 80, .height = 24 };
constexpr auto Narrow = Size { .width = 50, .height = 20 };

/// A finished run whose worktree was removed.
[[nodiscard]] Run finishedRun(std::string_view id, ItemRef item, std::string_view task = "fix_ci")
{
    return storedRun(Finished { .ended = Ended::Completed,
                                .fate = Fate::Removed,
                                .entries = {},
                                .seen = Seen::Opened,
                                .endedWallMs = DispatchWallMs },
                     id,
                     std::move(item),
                     task);
}

/// A world of one running agent on #2375 and one finished run on #2369, both listed.
[[nodiscard]] World transcriptWorld()
{
    return World { .stored = { storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                               finishedRun(FinishedId, ChangeRequestRef { 2369 }) } };
}

/// @return Now on the fixture's clock, in the engine's monotonic milliseconds.
[[nodiscard]] uint64_t monoOf(DashboardFixture const& fixture)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(fixture.clock.now().time_since_epoch())
            .count());
}

/// @return What the verb printed: @p stdoutText, exit 0.
[[nodiscard]] ProcessOutcome printed(std::string stdoutText)
{
    return ProcessOutcome { .start = StartResult::Started,
                            .completion = Completion::Exited,
                            .exitCode = 0,
                            .extent = OutputExtent::Whole,
                            .stdoutText = std::move(stdoutText),
                            .stderrText = {} };
}

/// @return What a failed verb printed: @p stderrText, exit 1.
[[nodiscard]] ProcessOutcome failedWith(std::string stderrText)
{
    return ProcessOutcome { .start = StartResult::Started,
                            .completion = Completion::Exited,
                            .exitCode = 1,
                            .extent = OutputExtent::Whole,
                            .stdoutText = {},
                            .stderrText = std::move(stderrText) };
}

/// @return The contract's page printed from the recorded log.
[[nodiscard]] std::string basicPage()
{
    return readFixture(FixtureRoot::Contract, "agent-transcript/format-1/basic-page-1.json");
}

/// What the guest does with an action of the dashboard: a transcript page asked for is answered with
/// @p answer (none: left unanswered); a board shown or hidden reaches the engine.
/// @return The `agent-transcript` commands the engine ran.
std::vector<RunCommand> carryOut(DashboardFixture& fixture,
                                 std::vector<UiAction> const& actions,
                                 std::optional<ProcessOutcome> const& answer)
{
    auto commands = std::vector<RunCommand> {};
    for (auto const& action: actions)
    {
        auto const now = monoOf(fixture);
        auto effects = std::vector<Effect> {};
        if (auto const* const read = std::get_if<ReadTranscriptAction>(&action))
            effects = fixture.engine.step(ReadTranscript { .run = read->run,
                                                           .tab = std::string { DashboardFixture::Tab },
                                                           .page = read->page,
                                                           .monoMs = now });
        else if (auto const* const show = std::get_if<ShowBoardAction>(&action))
            effects = fixture.engine.step(
                ShowBoard { .run = show->run, .tab = std::string { DashboardFixture::Tab }, .monoMs = now });
        else if (std::holds_alternative<HideBoardAction>(action))
            effects = fixture.engine.step(
                HideBoard { .tab = std::string { DashboardFixture::Tab }, .monoMs = now });
        for (auto const& effect: effects)
            if (auto const* const command = std::get_if<RunCommand>(&effect);
                command != nullptr && command->command == "agent-transcript")
            {
                commands.push_back(*command);
                if (answer)
                    static_cast<void>(fixture.engine.step(
                        ProcessFinished { .tag = command->tag, .outcome = *answer, .monoMs = now }));
            }
    }
    fixture.dashboard.engineChanged();
    return commands;
}

/// Selects @p run and presses `t`, as the user does, then carries the action out.
/// @return The actions `t` asked for.
std::vector<UiAction> openTranscript(DashboardFixture& fixture,
                                     std::string_view run,
                                     std::optional<ProcessOutcome> const& answer)
{
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(run).empty());
    REQUIRE(fixture.dashboard.selectedRun() == run);
    auto actions = fixture.dashboard.input("t");
    static_cast<void>(carryOut(fixture, actions, answer));
    return actions;
}

/// @return @p text as a JSON string literal.
[[nodiscard]] std::string jsonString(std::string_view text)
{
    auto quoted = std::string { "\"" };
    for (auto const c: text)
    {
        if (c == '"' || c == '\\')
            quoted += std::format("\\{}", c);
        else if (c == '\n')
            quoted += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20)
            quoted += std::format("\\u{:04x}", static_cast<unsigned>(c));
        else
            quoted += c;
    }
    return quoted + "\"";
}

/// @return The first @p count rows the transcript panel draws inside its frame: the frame, its padding and
///         each row's trailing blanks taken off.
[[nodiscard]] std::vector<std::string> contentRows(DashboardFixture& fixture, std::size_t count)
{
    auto rows = std::vector<std::string> {};
    for (auto const row: std::views::split(fixture.screen(), '\n') | std::views::drop(1))
    {
        auto text = std::string_view { row };
        if (rows.size() == count || !text.starts_with("│ "))
            break;
        text.remove_prefix(std::string_view { "│ " }.size());
        if (text.ends_with("│"))
            text.remove_suffix(std::string_view { "│" }.size());
        rows.emplace_back(text.substr(0, text.find_last_not_of(' ') + 1));
    }
    return rows;
}

/// @return Whether @p text is drawn on the dashboard's last frame, from its first cell, in @p role.
[[nodiscard]] bool isDrawnIn(DashboardFixture& fixture, std::string_view text, Role role)
{
    auto const screen = fixture.screen();
    auto row = 0;
    for (auto const line: std::views::split(screen, '\n'))
    {
        auto const drawn = std::string_view { line };
        if (auto const at = drawn.find(text); at != std::string_view::npos)
        {
            // Every cell before it holds one code point: count those, not bytes.
            auto const column = std::ranges::count_if(
                drawn.substr(0, at), [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; });
            // A leading blank is drawn in its line's style too: the first cell that is not one says it.
            auto const first = static_cast<int>(column) + static_cast<int>(text.find_first_not_of(' '));
            auto const style = core::tui::test::styleAt(fixture.dashboard.rendered(), row, first);
            auto const theme = dashboardTheme();
            auto const& wanted = styleOf(theme, role);
            return style.fg == wanted.fg && style.bg == wanted.bg && style.bold == wanted.bold
                   && style.dim == wanted.dim && style.italic == wanted.italic;
        }
        ++row;
    }
    return false;
}

/// One entry of a page written by a test.
struct PageEntry
{
    std::string_view kind = {};
    std::string text = {};
    std::optional<std::string_view> time = std::nullopt; ///< Its time; none: a minute after the one before.
    std::optional<int32_t> offset = std::nullopt;        ///< The offset the host printed beside it, if any.
};

/// @return A format-1 page of @p entries, as the verb prints one.
[[nodiscard]] std::string pageJson(std::vector<PageEntry> const& entries,
                                   uint32_t page = 1,
                                   uint32_t pages = 1,
                                   uint32_t notUnderstood = 0)
{
    auto json = std::format(R"({{"format":1,"session":"00000000-0000-4000-8000-000000000001",)"
                            R"("writtenBy":"2.1.328","page":{},"pages":{},"notUnderstood":{},"entries":[)",
                            page,
                            pages,
                            notUnderstood);
    auto minute = 0;
    for (auto const& [kind, text, time, offset]: entries)
    {
        if (minute > 0)
            json += ',';
        auto const at = time ? std::string { *time } : std::format("2026-11-19T18:{:02}:00.000Z", minute);
        ++minute;
        json += std::format(R"({{"kind":"{}","time":{},{}"text":{}}})",
                            kind,
                            jsonString(at),
                            offset ? std::format(R"("offset":{},)", *offset) : std::string {},
                            jsonString(text));
    }
    return json + "]}";
}

/// @return 8 KiB of mixed text: CJK, emoji, a 3 000-character line and an indented code block.
[[nodiscard]] std::vector<PageEntry> mixedEntries()
{
    auto longLine = std::string {};
    while (longLine.size() < 3000)
        longLine += "abcdefghij ";
    longLine.resize(3000);
    auto entries = std::vector<PageEntry> {
        { .kind = "user",
          .text = "日本語のテキストを折り返してください。漢字とかなが混ざった長い一文です。" },
        { .kind = "assistant", .text = "Done 🎉 — the build is green ✅, and the 🐛 is gone. 中文也可以。" },
        { .kind = "tool-call", .text = R"(Read {"file_path":"/home/user/proj/src/main.cpp"})" },
        { .kind = "tool-result",
          .text = "int main()\n{\n    for (auto const x: xs)\n        std::println(\"{}\", x);\n}" },
        { .kind = "tool-error", .text = "File does not exist." },
        { .kind = "note", .text = "conversation compacted" },
        { .kind = "assistant", .text = longLine },
    };
    auto size = std::size_t { 0 };
    for (auto const& entry: entries)
        size += entry.text.size();
    // Filled up to 8 KiB with a paragraph of mixed text.
    auto filler = std::string {};
    while (size + filler.size() < std::size_t { 8 } * 1024)
        filler += "Mixed 混合 text 🙂 that wraps. ";
    entries.push_back({ .kind = "assistant", .text = filler });
    return entries;
}

/// Draws @p panel on its own, on a canvas of @p size.
/// @return What it drew.
std::string drawnAlone(TranscriptPanel& panel, Size size)
{
    auto buffer = core::tui::Buffer { size.height, size.width };
    auto const theme = dashboardTheme();
    auto canvas = core::tui::Canvas {
        buffer, core::tui::Rect { .x = 0, .y = 0, .width = size.width, .height = size.height }, theme
    };
    panel.render(canvas);
    return core::tui::test::canvasToString(buffer);
}

/// @return The failed screen of the running agent's transcript, whose worktree is @p worktree, drawn at
///         @p size.
[[nodiscard]] std::string failedScreenWith(std::string worktree,
                                           Size size,
                                           std::vector<HostProgram> selfPrograms = {})
{
    auto world = transcriptWorld();
    world.stored.front().worktree = std::move(worktree);
    world.selfPrograms = std::move(selfPrograms);
    auto fixture = DashboardFixture { world, size };
    openTranscript(fixture, RunningId, failedWith("no session log\n"));
    return fixture.screen();
}

} // namespace

// {{{ the pure parts

TEST_CASE("an entry's time is shown in local time", "[orchestrator][view][transcript]")
{
    CHECK(localClockOf("2026-11-19T18:36:55.086Z", 0) == "18:36");
    CHECK(localClockOf("2026-11-19T18:36:55.086Z", 60) == "19:36");
    CHECK(localClockOf("2026-11-19T23:30:00.000Z", 60) == "00:30");
    CHECK(localClockOf("2026-11-19T00:15:00.000Z", -480) == "16:15");
    CHECK(localClockOf("", 0) == "--:--");
    CHECK(localClockOf("2026-11-19T1x:36", 0) == "--:--");
    CHECK(localClockOf("2026-11-19T25:00", 0) == "--:--");
}

TEST_CASE("an entry's header names its kind", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(
        fixture,
        RunningId,
        printed(pageJson({ { .kind = "user", .text = "hi", .offset = 0 },
                           { .kind = "tool-call", .text = R"(Bash {"command":"ls"})", .offset = 0 },
                           { .kind = "note", .text = "conversation compacted", .time = "" } })));

    CHECK(contentRows(fixture, 8)
          == std::vector<std::string> { "you · 18:00",
                                        "  hi",
                                        "",
                                        "tool Bash · 18:01",
                                        R"(  {"command":"ls"})",
                                        "",
                                        "note · --:--",
                                        "  conversation compacted" });
    CHECK(isDrawnIn(fixture, "you · 18:00", Role::Accent));
    CHECK(isDrawnIn(fixture, "  hi", Role::Text));
    CHECK(isDrawnIn(fixture, "tool Bash · 18:01", Role::Dim));
    CHECK(isDrawnIn(fixture, "note · --:--", Role::Note));
}

TEST_CASE("each entry's time is shown in the local time its instant had", "[orchestrator][view][transcript]")
{
    // Daylight saving ends in Berlin at 01:00 UTC on 2026-10-25: the host printed each entry's own offset.
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(
        fixture,
        RunningId,
        printed(pageJson(
            { { .kind = "user", .text = "summer", .time = "2026-10-25T00:30:00.000Z", .offset = 120 },
              { .kind = "user", .text = "winter", .time = "2026-10-25T01:30:00.000Z", .offset = 60 },
              { .kind = "user", .text = "unknown", .time = "2026-10-25T01:45:00.000Z" } })));

    auto const rows = contentRows(fixture, 8);
    CHECK(rows[0] == "you · 02:30");
    CHECK(rows[3] == "you · 02:30");
    // Without the offset its instant had, the time is shown in UTC, and says so.
    CHECK(rows[6] == "you · 01:45 UTC");
}

TEST_CASE("the footer names the writer and what was not understood", "[orchestrator][view][transcript]")
{
    auto page =
        TranscriptPage { .page = 1, .pages = 1, .writtenBy = "2.1.328", .notUnderstood = 0, .entries = {} };
    CHECK(transcriptFooterOf(page) == "written by Claude Code 2.1.328");
    page.notUnderstood = 1;
    CHECK(transcriptFooterOf(page) == "written by Claude Code 2.1.328 · 1 entry not understood");
    page.notUnderstood = 2;
    CHECK(transcriptFooterOf(page) == "written by Claude Code 2.1.328 · 2 entries not understood");
    page.writtenBy.clear();
    page.notUnderstood = 0;
    CHECK(transcriptFooterOf(page).empty());
}

TEST_CASE("the command to read a page by hand is quoted", "[orchestrator][view][transcript]")
{
    auto run = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    run.worktree = "/home/u/work trees/x";
    CHECK(transcriptCommandOf(run, 1, {})
          == std::vector<std::string> { "contour agent transcript \\",
                                        std::format("--session {} \\", run.session),
                                        "--cwd '/home/u/work trees/x'" });
    auto const third = transcriptCommandOf(run, 3, {});
    REQUIRE(third.has_value());
    CHECK(third->back() == "--page 3");
}

TEST_CASE("the command to read a page by hand names the contour the host runs",
          "[orchestrator][view][transcript]")
{
    // A .dmg's contour is on no shell's PATH: the command names the one the host ran.
    constexpr auto Bundled = "/Applications/Contour Beta.app/Contents/MacOS/contour"sv;
    auto const programs = std::vector { HostProgram { .name = "contour", .path = std::string { Bundled } } };
    auto run = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    auto const command = transcriptCommandOf(run, 1, programs);
    REQUIRE(command.has_value());
    CHECK(command->front() == std::format("'{}' agent transcript \\", Bundled));

    auto const screen = failedScreenWith(run.worktree, Wide, programs);
    CHECK(screen.contains("To read it by hand, run:"));
    CHECK(screen.contains(std::format("'{}' agent transcript \\", Bundled)));

    // A path that is drawn differently from what it is is never shown.
    auto const hidden =
        std::vector { HostProgram { .name = "contour", .path = "/opt/con\u200Btour/contour" } };
    CHECK(!transcriptCommandOf(run, 1, hidden).has_value());
    CHECK(failedScreenWith(run.worktree, Wide, hidden).contains("cannot be shown safely"));
}

TEST_CASE("the command to read a page by hand is refused when a value is drawn differently",
          "[orchestrator][view][transcript]")
{
    auto run = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    // A zero-width space draws as nothing; a bidirectional mark is dropped when drawn.
    for (auto const* const mark: { "\u200B", "\u200F" })
    {
        CAPTURE(mark);
        run.worktree = std::format("/home/u/worktrees/contour/pr-2375{}", mark);
        CHECK(!transcriptCommandOf(run, 1, {}).has_value());
        auto const screen = failedScreenWith(run.worktree, Medium);
        CHECK(screen.contains("cannot be shown safely"));
        CHECK(!screen.contains("To read it by hand, run:"));
        CHECK(!screen.contains("--cwd"));
    }
}

TEST_CASE("a command line too long for the panel is never broken or cut", "[orchestrator][view][transcript]")
{
    // No other text on the screen holds a Q: each is a character of the path.
    auto const path = std::format("/home/u/worktrees/contour/{}-end", std::string(200, 'Q'));
    auto run = storedRun(Running {}, RunningId, ChangeRequestRef { 2375 });
    run.worktree = path;
    auto const command = transcriptCommandOf(run, 1, {});
    REQUIRE(command.has_value());
    CHECK(command->back() == std::format("--cwd {}", path));
    // A copy of a broken line would carry a newline inside the path, and a cut one another path: the line
    // that does not fit asks for a wider window instead, and the lines that fit are shown as they are.
    auto const screen = failedScreenWith(path, Medium);
    CHECK(screen.contains("To read it by hand, run:"));
    CHECK(screen.contains("contour agent transcript \\"));
    CHECK(screen.contains(std::format("--session {} \\", run.session)));
    CHECK(screen.contains(WidenToSeeText));
    CHECK(!screen.contains("--cwd"));
    CHECK(!screen.contains("QQ-end"));
    checkGolden("transcript-failed-long-cwd-80x24", screen);
}

// }}}
// {{{ golden screens

TEST_CASE("transcript golden screens", "[orchestrator][view][transcript]")
{
    struct Scene
    {
        std::string_view name = {};
        std::optional<ProcessOutcome> answer = std::nullopt;
        std::vector<Size> sizes = {};
    };
    auto notUnderstood = basicPage();
    auto const field = std::string_view { R"("notUnderstood":0)" };
    notUnderstood.replace(notUnderstood.find(field), field.size(), R"("notUnderstood":2)");
    auto const scenes = std::array {
        Scene { .name = "transcript", .answer = printed(basicPage()), .sizes = { Wide, Medium, Narrow } },
        Scene {
            .name = "transcript-mixed-8k", .answer = printed(pageJson(mixedEntries())), .sizes = { Wide } },
        Scene { .name = "transcript-loading", .answer = std::nullopt, .sizes = { Medium } },
        Scene { .name = "transcript-failed",
                .answer = failedWith(
                    std::format("no session log for {} in {}/contour/{} under /home/u/.claude/projects\n",
                                storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }).session,
                                WorktreeRoot,
                                RunningId)),
                .sizes = { Medium } },
        Scene { .name = "transcript-format-mismatch",
                .answer = failedWith("unsupported transcript format 1; this contour prints 2\n"),
                .sizes = { Medium } },
        Scene { .name = "transcript-not-understood", .answer = printed(notUnderstood), .sizes = { Medium } },
    };
    for (auto const& scene: scenes)
        for (auto const size: scene.sizes)
        {
            auto const name = std::format("{}-{}x{}", scene.name, size.width, size.height);
            SECTION(name)
            {
                auto fixture = DashboardFixture { transcriptWorld(), size };
                openTranscript(fixture, RunningId, scene.answer);
                checkGolden(name, fixture.screen());
            }
        }
}

TEST_CASE("a transcript's escapes are drawn inert", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Wide };
    openTranscript(
        fixture,
        RunningId,
        printed(pageJson({ { .kind = "assistant", .text = "ok\x1b]52;c;aGk=\x07 copied \x1b[2Jcleared" },
                           { .kind = "tool-call", .text = "Ba\x1b[31msh {}" } })));
    auto const screen = fixture.screen();
    CHECK(screen.contains("copied"));
    CHECK(screen.contains("cleared"));
    CHECK_FALSE(screen.contains('\x1b'));
    CHECK_FALSE(screen.contains('\x07'));
}

// }}}
// {{{ keys

TEST_CASE("t opens the selected run's transcript on page 1", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(FinishedId).empty());
    auto const actions = fixture.dashboard.input("t");
    CHECK(actions
          == std::vector<UiAction> { ReadTranscriptAction { .run = std::string { FinishedId }, .page = 1 } });
    auto const& interaction = fixture.dashboard.interaction();
    CHECK(interaction.screen == Screen::Transcript);
    CHECK(interaction.subject == FinishedId);
    CHECK(interaction.page == 1);

    // The engine runs the verb for the run's session in its worktree, also when the worktree is gone.
    auto const commands = carryOut(fixture, actions, printed(basicPage()));
    REQUIRE(commands.size() == 1);
    auto const screen = fixture.screen();
    CHECK(screen.contains(std::format("Transcript · {} · page 1 of 1", FinishedId)));
    CHECK(screen.contains("written by Claude Code 2.1.328"));
    CHECK(screen.contains("←→ page"));

    // Esc returns to the dashboard, asking nothing: a transcript is read only when a key asks for a page.
    CHECK(fixture.dashboard.input("\x1b").empty());
    CHECK(fixture.escapeTimesOut().empty());
    CHECK(fixture.dashboard.interaction().screen == Screen::Dashboard);
    CHECK(fixture.dashboard.interaction().subject.empty());
}

TEST_CASE("right asks for the next page, not past the last", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(pageJson({ { .kind = "user", .text = "one" } }, 1, 3)));
    // Left on the first page asks for nothing.
    CHECK(fixture.dashboard.input("\x1b[D").empty());

    auto actions = fixture.dashboard.input("\x1b[C");
    CHECK(actions
          == std::vector<UiAction> { ReadTranscriptAction { .run = std::string { RunningId }, .page = 2 } });
    // While page 2 is read, the pages are known from page 1.
    CHECK(fixture.screen().contains("reading the transcript…"));
    static_cast<void>(
        carryOut(fixture, actions, printed(pageJson({ { .kind = "user", .text = "two" } }, 2, 3))));
    CHECK(fixture.screen().contains("page 2 of 3"));

    actions = fixture.dashboard.input("\x1b[C");
    CHECK(actions
          == std::vector<UiAction> { ReadTranscriptAction { .run = std::string { RunningId }, .page = 3 } });
    static_cast<void>(
        carryOut(fixture, actions, printed(pageJson({ { .kind = "user", .text = "three" } }, 3, 3))));

    // Past the last page there is nothing to ask for; the page shown is not asked for again.
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    CHECK(fixture.dashboard.input("\x1b[F").empty());
    CHECK(fixture.dashboard.interaction().page == 3);

    CHECK(fixture.dashboard.input("\x1b[D")
          == std::vector<UiAction> { ReadTranscriptAction { .run = std::string { RunningId }, .page = 2 } });
}

TEST_CASE("end asks for the last page", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(pageJson({ { .kind = "user", .text = "one" } }, 1, 5)));
    auto const actions = fixture.dashboard.input("\x1b[F");
    CHECK(actions
          == std::vector<UiAction> { ReadTranscriptAction { .run = std::string { RunningId }, .page = 5 } });
    static_cast<void>(
        carryOut(fixture, actions, printed(pageJson({ { .kind = "user", .text = "five" } }, 5, 5))));
    CHECK(fixture.screen().contains("page 5 of 5"));
    // Home goes back to the first.
    CHECK(fixture.dashboard.input("\x1b[H")
          == std::vector<UiAction> { ReadTranscriptAction { .run = std::string { RunningId }, .page = 1 } });
}

TEST_CASE("paging waits for a page's count before it goes past the first", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, std::nullopt);
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    CHECK(fixture.dashboard.input("\x1b[F").empty());
    CHECK(fixture.dashboard.interaction().page == 1);
}

TEST_CASE("the transcript scrolls by line and by screen", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    auto entries = std::vector<PageEntry> {};
    for (auto const index: std::views::iota(1, 31))
        entries.push_back({ .kind = "assistant", .text = std::format("entry {}", index) });
    openTranscript(fixture, RunningId, printed(pageJson(entries)));
    CHECK(fixture.screen().contains("entry 1 "));
    auto const& interaction = fixture.dashboard.interaction();
    CHECK(fixture.dashboard.input("j").empty());
    CHECK(interaction.scroll == 1);
    CHECK(fixture.dashboard.input("\x1b[6~").empty());
    CHECK(interaction.scroll > 10);
    CHECK(fixture.dashboard.input("\x1b[5~k").empty());
    CHECK(interaction.scroll == 0);
}

TEST_CASE("a page of 1000 entries wraps only what is in view", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Wide };
    auto entries = std::vector<PageEntry> {};
    for (auto const index: std::views::iota(0, 1000))
        entries.push_back({ .kind = "assistant", .text = std::format("entry {} of a long page", index) });
    openTranscript(fixture, RunningId, printed(pageJson(entries)));

    // The panel drawn on its own, so that what it wrapped can be counted.
    auto interaction = fixture.dashboard.interaction();
    auto const tab = std::string { DashboardFixture::Tab };
    auto state = TranscriptPanelState {};
    auto panel = TranscriptPanel { fixture.engine, interaction, tab, state };
    auto buffer = core::tui::Buffer { Wide.height, Wide.width };
    auto const theme = dashboardTheme();
    auto canvas = core::tui::Canvas {
        buffer, core::tui::Rect { .x = 0, .y = 0, .width = Wide.width, .height = Wide.height }, theme
    };
    panel.render(canvas);
    // Three lines an entry: what fills the view and no more.
    CHECK(state.wrappedEntries < 20);
    CHECK(state.lines == UnknownLineCount);

    // Drawn again, nothing is wrapped again; scrolled down a screen, only the entries scrolled to are.
    auto const first = state.wrappedEntries;
    panel.render(canvas);
    CHECK(state.wrappedEntries == first);
    interaction.scroll = 30;
    panel.render(canvas);
    CHECK(state.wrappedEntries > first);
    CHECK(state.wrappedEntries < 35);
    // Scrolled to the end, the whole page is wrapped, once.
    interaction.scroll = UnknownLineCount;
    panel.render(canvas);
    CHECK(state.wrappedEntries == 1000);
    CHECK(state.lines == (1000 * 3) - 1);
}

TEST_CASE("the transcript is wrapped anew for a new width", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Wide };
    auto entries = std::vector<PageEntry> {};
    for (auto const index: std::views::iota(0, 3))
        entries.push_back(
            { .kind = "assistant", .text = std::format("entry {} {}end", index, std::string(150, 'w')) });
    openTranscript(fixture, RunningId, printed(pageJson(entries)));
    auto const interaction = fixture.dashboard.interaction();
    auto const tab = std::string { DashboardFixture::Tab };
    auto state = TranscriptPanelState {};
    auto panel = TranscriptPanel { fixture.engine, interaction, tab, state };

    static_cast<void>(drawnAlone(panel, Wide));
    CHECK(state.wrappedEntries == 3);
    auto const wideLines = state.lines;
    // Narrower, every entry is wrapped again, to more lines; and back, again.
    static_cast<void>(drawnAlone(panel, Narrow));
    CHECK(state.wrappedEntries == 6);
    CHECK(state.lines > wideLines);
    static_cast<void>(drawnAlone(panel, Wide));
    CHECK(state.wrappedEntries == 9);
    CHECK(state.lines == wideLines);
}

TEST_CASE("the transcript is wrapped anew for a new page", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Wide };
    openTranscript(fixture,
                   RunningId,
                   printed(pageJson({ { .kind = "assistant", .text = "on the first page" } }, 1, 2)));
    auto interaction = fixture.dashboard.interaction();
    auto const tab = std::string { DashboardFixture::Tab };
    auto state = TranscriptPanelState {};
    auto panel = TranscriptPanel { fixture.engine, interaction, tab, state };
    CHECK(drawnAlone(panel, Wide).contains("on the first page"));
    CHECK(state.wrappedEntries == 1);

    // The same panel, the same width: the second page is wrapped, not the first one's lines kept.
    static_cast<void>(
        carryOut(fixture,
                 { UiAction { ReadTranscriptAction { .run = std::string { RunningId }, .page = 2 } } },
                 printed(pageJson({ { .kind = "assistant", .text = "on the second page" } }, 2, 2))));
    interaction.page = 2;
    auto const second = drawnAlone(panel, Wide);
    CHECK(second.contains("on the second page"));
    CHECK(!second.contains("on the first page"));
    CHECK(state.wrappedEntries == 2);
}

TEST_CASE("the transcript's end is reached by scrolling, and the scroll is kept within it",
          "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    auto entries = std::vector<PageEntry> {};
    for (auto const index: std::views::iota(1, 31))
        entries.push_back({ .kind = "assistant", .text = std::format("entry {}", index) });
    openTranscript(fixture, RunningId, printed(pageJson(entries)));
    auto const& interaction = fixture.dashboard.interaction();
    static_cast<void>(fixture.screen());
    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
        CHECK(fixture.dashboard.input("\x1b[6~").empty());
    auto const screen = fixture.screen();
    CHECK(screen.contains("entry 30"));
    // The draw that found the end keeps the scroll there: one line up shows one line more above.
    auto const last = interaction.scroll;
    CHECK(last > 0);
    CHECK(last < UnknownLineCount);
    CHECK(fixture.dashboard.input("k").empty());
    CHECK(interaction.scroll == last - 1);
}

TEST_CASE("t from a board keeps the subject run", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    static_cast<void>(carryOut(fixture, fixture.dashboard.input("b"), std::nullopt));
    REQUIRE(fixture.dashboard.interaction().screen == Screen::Board);
    CHECK(fixture.screen().contains("t transcript"));

    // The board is left, so no longer read; the transcript of its run is asked for.
    auto const actions = fixture.dashboard.input("t");
    CHECK(actions
          == std::vector<UiAction> { HideBoardAction {},
                                     ReadTranscriptAction { .run = std::string { RunningId }, .page = 1 } });
    CHECK(fixture.dashboard.interaction().screen == Screen::Transcript);
    CHECK(fixture.dashboard.interaction().subject == RunningId);

    // And `b` goes back to the board of the same run.
    CHECK(fixture.dashboard.input("b")
          == std::vector<UiAction> { ShowBoardAction { .run = std::string { RunningId } } });
    CHECK(fixture.dashboard.interaction().screen == Screen::Board);
    CHECK(fixture.dashboard.interaction().subject == RunningId);
}

TEST_CASE("b on a transcript shows no board once the run's worktree was removed",
          "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(FinishedId).empty());
    static_cast<void>(carryOut(fixture, fixture.dashboard.input("t"), printed(basicPage())));
    REQUIRE(fixture.dashboard.interaction().screen == Screen::Transcript);
    auto const screen = fixture.screen();
    CHECK(screen.contains("esc back"));
    CHECK_FALSE(screen.contains("b board"));
    CHECK(fixture.dashboard.input("b").empty());
    CHECK(fixture.dashboard.interaction().screen == Screen::Transcript);
}

TEST_CASE("esc returns to where the transcript was opened from", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RunningId).empty());
    static_cast<void>(carryOut(fixture, fixture.dashboard.input("b"), std::nullopt));
    static_cast<void>(carryOut(fixture, fixture.dashboard.input("t"), printed(basicPage())));
    REQUIRE(fixture.dashboard.interaction().screen == Screen::Transcript);

    // Opened from the board: Esc goes back to the board, which is read again; Esc there to the dashboard.
    CHECK(fixture.dashboard.input("\x1b").empty());
    CHECK(fixture.escapeTimesOut()
          == std::vector<UiAction> { ShowBoardAction { .run = std::string { RunningId } } });
    CHECK(fixture.dashboard.interaction().screen == Screen::Board);
    checkGolden("transcript-back-to-board-80x24", fixture.screen());
    CHECK(fixture.dashboard.input("\x1b").empty());
    CHECK(fixture.escapeTimesOut() == std::vector<UiAction> { HideBoardAction {} });
    CHECK(fixture.dashboard.interaction().screen == Screen::Dashboard);

    // A board opened from a transcript goes back to it, its page kept and not read again.
    static_cast<void>(carryOut(fixture, fixture.dashboard.input("t"), printed(basicPage())));
    static_cast<void>(carryOut(fixture, fixture.dashboard.input("b"), std::nullopt));
    REQUIRE(fixture.dashboard.interaction().screen == Screen::Board);
    CHECK(fixture.dashboard.input("\x1b").empty());
    CHECK(fixture.escapeTimesOut() == std::vector<UiAction> { HideBoardAction {} });
    CHECK(fixture.dashboard.interaction().screen == Screen::Transcript);
    CHECK(fixture.screen().contains("written by Claude Code"));
    CHECK(fixture.dashboard.input("\x1b").empty());
    CHECK(fixture.escapeTimesOut().empty());
    CHECK(fixture.dashboard.interaction().screen == Screen::Dashboard);
}

TEST_CASE("two tabs page one run, each on its own page", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(pageJson({ { .kind = "user", .text = "one" } }, 1, 3)));
    // Another tab reads page 3 of the same run meanwhile.
    auto const now = monoOf(fixture);
    for (auto const& effect: fixture.engine.step(
             ReadTranscript { .run = std::string { RunningId }, .tab = "tab-2", .page = 3, .monoMs = now }))
        if (auto const* const command = std::get_if<RunCommand>(&effect))
            static_cast<void>(fixture.engine.step(
                ProcessFinished { .tag = command->tag,
                                  .outcome = printed(pageJson({ { .kind = "user", .text = "three" } }, 3, 3)),
                                  .monoMs = now }));
    fixture.dashboard.engineChanged();
    REQUIRE(fixture.engine.transcript("tab-2", RunningId)->page == 3);
    auto const screen = fixture.screen();
    CHECK(screen.contains("page 1 of 3"));
    CHECK(screen.contains("one"));
    CHECK_FALSE(screen.contains("three"));
}

TEST_CASE("a transcript the engine no longer keeps for the tab is asked for again",
          "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(pageJson({ { .kind = "user", .text = "one" } }, 1, 1)));
    CHECK(fixture.dashboard.dueActions().empty());
    // The engine dropped what it kept for this tab, as for a tab that closed.
    static_cast<void>(fixture.engine.step(
        TabClosed { .tab = std::string { DashboardFixture::Tab }, .monoMs = monoOf(fixture) }));
    fixture.dashboard.engineChanged();
    REQUIRE(fixture.engine.transcript(DashboardFixture::Tab, RunningId) == nullptr);

    auto const due = fixture.dashboard.dueActions();
    CHECK(due
          == std::vector<UiAction> { ReadTranscriptAction { .run = std::string { RunningId }, .page = 1 } });
    carryOut(fixture, due, printed(pageJson({ { .kind = "user", .text = "one" } }, 1, 1)));
    CHECK(fixture.dashboard.dueActions().empty());
    auto const screen = fixture.screen();
    CHECK(screen.contains("one"));
    CHECK_FALSE(screen.contains("reading the transcript"));
}

TEST_CASE("t is inert on an orphan row", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { World {
        .prs = { "github/prs-empty.json" },
        .worktrees = { WorktreeEntry { .path = "/home/u/worktrees/contour/pr-2-20260101-000000",
                                       .branch = std::nullopt } } } };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.input("\x1b[F").empty());
    REQUIRE(fixture.dashboard.selectedRun().empty());
    CHECK(fixture.dashboard.input("t").empty());
    CHECK(fixture.dashboard.interaction().screen == Screen::Dashboard);
}

TEST_CASE("enter on a transcript reveals an active run's tab", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(basicPage()));
    CHECK(fixture.screen().contains("enter agent tab"));
    CHECK(fixture.dashboard.input("\r")
          == std::vector<UiAction> { RevealAgentAction { .run = std::string { RunningId } } });

    // A finished run has no tab to go to.
    CHECK(fixture.dashboard.select(FinishedId).empty());
    static_cast<void>(carryOut(fixture, fixture.dashboard.input("t"), printed(basicPage())));
    CHECK_FALSE(fixture.screen().contains("enter agent tab"));
    CHECK(fixture.dashboard.input("\r").empty());
}

TEST_CASE("selecting a row leaves a transcript", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(basicPage()));
    CHECK(fixture.dashboard.select(FinishedId).empty());
    CHECK(fixture.dashboard.interaction().screen == Screen::Dashboard);
    CHECK(fixture.dashboard.selectedRun() == FinishedId);
}

// }}}
// {{{ a new run on the item

TEST_CASE("d on a Finished row opens the form on its item and task", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(FinishedId).empty());
    CHECK(fixture.dashboard.input("d").empty());
    auto const& form = fixture.dashboard.form();
    REQUIRE(form.has_value());
    CHECK(form->repo == "contour");
    CHECK(form->item == ItemRef { ChangeRequestRef { 2369 } });
    CHECK(form->task == 0); // fix_ci
    CHECK_FALSE(form->session.empty());
    // Nothing starts before the form is confirmed.
    CHECK(fixture.screen().contains("Dispatch an agent"));

    SECTION("from its transcript")
    {
        CHECK(fixture.dashboard.input("\x1b").empty());
        static_cast<void>(fixture.escapeTimesOut());
        REQUIRE_FALSE(fixture.dashboard.form().has_value());
        static_cast<void>(carryOut(fixture, fixture.dashboard.input("t"), printed(basicPage())));
        CHECK(fixture.screen().contains("d new run"));
        CHECK(fixture.dashboard.input("d").empty());
        REQUIRE(fixture.dashboard.form().has_value());
        CHECK(fixture.dashboard.form()->item == ItemRef { ChangeRequestRef { 2369 } });
        CHECK(fixture.dashboard.interaction().screen == Screen::Transcript);
    }
}

TEST_CASE("d offers the item's default task when the run's task is gone", "[orchestrator][view][transcript]")
{
    constexpr auto RetiredId = "pr-2346-20261113-140211"sv;
    auto world = transcriptWorld();
    world.stored.push_back(finishedRun(RetiredId, ChangeRequestRef { 2346 }, "retired_task"));
    auto fixture = DashboardFixture { world, Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RetiredId).empty());
    CHECK(fixture.dashboard.input("d").empty());
    REQUIRE(fixture.dashboard.form().has_value());
    CHECK(fixture.dashboard.form()->item == ItemRef { ChangeRequestRef { 2346 } });
    CHECK(fixture.dashboard.form()->task == 0); // fix_ci, the first task for a pull request
}

TEST_CASE("d on an item with an active run shows the refusal", "[orchestrator][view][transcript]")
{
    constexpr auto EarlierId = "pr-2375-20261112-100000"sv;
    auto world = transcriptWorld();
    world.stored.push_back(finishedRun(EarlierId, ChangeRequestRef { 2375 }));
    auto fixture = DashboardFixture { world, Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(EarlierId).empty());
    CHECK(fixture.dashboard.input("d").empty());
    REQUIRE(fixture.dashboard.form().has_value());
    CHECK(fixture.screen().contains("refused"));
    // Enter confirms nothing the preview refuses.
    CHECK(fixture.dashboard.input("\r").empty());
}

TEST_CASE("d on an item no longer listed says so", "[orchestrator][view][transcript]")
{
    auto world = transcriptWorld();
    world.prs = { "github/prs-empty.json" };
    auto fixture = DashboardFixture { world, Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(FinishedId).empty());
    CHECK(fixture.dashboard.input("d").empty());
    CHECK_FALSE(fixture.dashboard.form().has_value());
    CHECK(fixture.dashboard.interaction().notice == "#2369 is not listed any more");
}

// }}}
