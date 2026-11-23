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
#include <iterator>
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

/// @return The contract's page printed from the recorded log.
[[nodiscard]] std::string basicPage()
{
    return readFixture(FixtureRoot::Contract, "agent-transcript/format-1/basic-page-1.json");
}

/// Selects @p run and shows its Transcript tab, the Focus given the keys, as the user does; its page is read,
/// answered with @p answer (none: left unanswered).
void openTranscript(DashboardFixture& fixture,
                    std::string_view run,
                    std::optional<ProcessOutcome> const& answer)
{
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(run).empty());
    REQUIRE(fixture.dashboard.selectedRun() == run);
    CHECK(fixture.dashboard.input("t\t").empty());
    static_cast<void>(fixture.settle(Answers { .transcript = answer }));
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

/// @return The first @p count rows the Transcript tab draws, below the Focus's tab strip: the border, its
///         padding and each row's trailing blanks taken off.
[[nodiscard]] std::vector<std::string> contentRows(DashboardFixture& fixture, std::size_t count)
{
    auto rows = std::vector<std::string> {};
    auto inTab = false;
    for (auto const row: std::views::split(fixture.screen(), '\n'))
    {
        auto text = std::string_view { row };
        if (!inTab)
        {
            inTab = text.contains("Board  Transcript  Details");
            continue;
        }
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

    // Wide enough for the Focus beside the Fleet to show its first line whole.
    auto const screen = failedScreenWith(run.worktree, Size { .width = 160, .height = 40 }, programs);
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

TEST_CASE("a transcript's escapes are drawn inert, and its URLs are hyperlinks",
          "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Wide };
    openTranscript(
        fixture,
        RunningId,
        printed(pageJson({ { .kind = "assistant", .text = "ok\x1b]52;c;aGk=\x07 copied \x1b[2Jcleared" },
                           { .kind = "assistant", .text = "opened https://github.com/o/r/pull/9 for review" },
                           { .kind = "tool-call", .text = "Ba\x1b[31msh {}" } })));
    auto const screen = fixture.screen();
    CHECK(screen.contains("copied"));
    CHECK(screen.contains("cleared"));
    CHECK_FALSE(screen.contains('\x1b'));
    CHECK_FALSE(screen.contains('\x07'));
    CHECK(std::ranges::any_of(
        fixture.dashboard.rendered().hyperlinks(),
        [](core::tui::HyperlinkRegion const& link) { return link.uri == "https://github.com/o/r/pull/9"; }));
}

// }}}
// {{{ keys

TEST_CASE("t shows the selected run's transcript, read for its session in its worktree",
          "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(FinishedId).empty());
    // A finished run opens on its Details; its transcript is read all the same, for latest.
    auto const commands = fixture.settle(Answers { .transcript = printed(basicPage()) });
    REQUIRE(commands.size() == 1);
    auto const& arguments = commands.front().arguments;
    auto const valueOf = [&](std::string_view name) {
        auto const found = std::ranges::find(arguments, name, &Argument::name);
        return found != arguments.end() ? found->value : std::string {};
    };
    CHECK(valueOf("page") == "1");
    CHECK(valueOf("path") == std::format("{}/contour/{}", WorktreeRoot, FinishedId));
    CHECK(fixture.dashboard.input("t").empty());
    auto const screen = fixture.screen();
    CHECK(screen.contains("page 1 of 1 · written by Claude Code 2.1.328"));
    CHECK(screen.contains("claude · 18:37"));
    // Nothing is read again for a finished run: its log no longer grows.
    CHECK(fixture.settle().empty());
}

TEST_CASE("right and left turn the transcript's pages, read as they are turned to",
          "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(pageJson({ { .kind = "user", .text = "one" } }, 1, 3)));
    // Following the agent, its last page is read once the count is known.
    auto const last = fixture.settle(
        Answers { .transcript = printed(pageJson({ { .kind = "user", .text = "three" } }, 3, 3)) });
    REQUIRE(last.size() == 1);
    CHECK(fixture.screen().contains("page 3 of 3"));
    // Right on the last page turns no page: it shows the tab to the right, and left comes back.
    CHECK(fixture.dashboard.input("\x1b[C").empty());
    CHECK(fixture.screen().contains("page 3 of 3") == false);
    CHECK(fixture.dashboard.input("\x1b[D").empty());
    CHECK(fixture.screen().contains("page 3 of 3"));
    CHECK(fixture.settle().empty());

    CHECK(fixture.dashboard.input("\x1b[D").empty());
    CHECK(fixture.dashboard.interaction().page == 2);
    CHECK(fixture.dashboard.interaction().follow == Follow::Away);
    REQUIRE(
        fixture
            .settle(Answers { .transcript = printed(pageJson({ { .kind = "user", .text = "two" } }, 2, 3)) })
            .size()
        == 1);
    auto const screen = fixture.screen();
    CHECK(screen.contains("page 2 of 3"));
    CHECK(screen.contains("↓ live"));
}

TEST_CASE("the transcript scrolls by screen, from its end", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Wide };
    auto entries = std::vector<PageEntry> {};
    for (auto const index: std::views::iota(1, 31))
        entries.push_back({ .kind = "assistant", .text = std::format("entry {}", index) });
    openTranscript(fixture, RunningId, printed(pageJson(entries)));
    // Following the agent, it shows its end.
    CHECK(fixture.screen().contains("entry 30"));
    auto const& interaction = fixture.dashboard.interaction();
    CHECK(fixture.dashboard.input("\x1b[5~").empty());
    CHECK(interaction.follow == Follow::Away);
    CHECK(interaction.scroll > 0);
    for ([[maybe_unused]] auto const _: std::views::iota(0, 5))
        CHECK(fixture.dashboard.input("\x1b[5~").empty());
    CHECK(interaction.scroll == 0);
    CHECK(fixture.screen().contains("entry 1 "));
}

TEST_CASE("a page of 1000 entries wraps only what is in view", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Wide };
    auto entries = std::vector<PageEntry> {};
    for (auto const index: std::views::iota(0, 1000))
        entries.push_back({ .kind = "assistant", .text = std::format("entry {} of a long page", index) });
    openTranscript(fixture, RunningId, printed(pageJson(entries)));

    // The panel drawn on its own, scrolled to its top, so that what it wrapped can be counted.
    auto subject =
        FocusSubject { .run = std::string { RunningId }, .page = 1, .scroll = 0, .follow = Follow::Away };
    auto const tab = std::string { DashboardFixture::Tab };
    auto state = TranscriptPanelState {};
    auto panel = TranscriptPanel { fixture.engine, subject, tab, state };
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
    subject.scroll = 30;
    panel.render(canvas);
    CHECK(state.wrappedEntries > first);
    CHECK(state.wrappedEntries < 35);
    // Following the agent, the whole page is wrapped, once.
    subject.follow = Follow::Live;
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
    auto const subject = FocusSubject { .run = std::string { RunningId }, .page = 1 };
    auto const tab = std::string { DashboardFixture::Tab };
    auto state = TranscriptPanelState {};
    auto panel = TranscriptPanel { fixture.engine, subject, tab, state };

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
    auto subject = FocusSubject { .run = std::string { RunningId }, .page = 1 };
    auto const tab = std::string { DashboardFixture::Tab };
    auto state = TranscriptPanelState {};
    auto panel = TranscriptPanel { fixture.engine, subject, tab, state };
    CHECK(drawnAlone(panel, Wide).contains("on the first page"));
    CHECK(state.wrappedEntries == 1);

    // The same panel, the same width: the second page is wrapped, not the first one's lines kept.
    static_cast<void>(fixture.carryOut(
        { UiAction { ReadTranscriptAction { .run = std::string { RunningId }, .page = 2 } } },
        Answers { .transcript =
                      printed(pageJson({ { .kind = "assistant", .text = "on the second page" } }, 2, 2)) }));
    subject.page = 2;
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
        CHECK(fixture.dashboard.input("\x1b[5~").empty());
    CHECK(interaction.scroll == 0);
    static_cast<void>(fixture.screen());
    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
        CHECK(fixture.dashboard.input("\x1b[6~").empty());
    auto const screen = fixture.screen();
    CHECK(screen.contains("entry 30"));
    // At the end it follows the agent again; a screen up stops following.
    CHECK(interaction.follow == Follow::Live);
    CHECK(fixture.dashboard.input("\x1b[5~").empty());
    CHECK(interaction.follow == Follow::Away);
    CHECK(interaction.scroll > 0);
    CHECK(interaction.scroll < UnknownLineCount);
}

TEST_CASE("two tabs page one run, each on its own page", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(pageJson({ { .kind = "user", .text = "one" } }, 1, 1)));
    // Another tab reads page 3 of the same run meanwhile.
    auto const now = fixture.monoMs();
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
    CHECK(screen.contains("page 1 of 1"));
    CHECK(screen.contains("one"));
    CHECK_FALSE(screen.contains("three"));
}

TEST_CASE("a transcript the engine no longer keeps for the tab is asked for again",
          "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(pageJson({ { .kind = "user", .text = "one" } }, 1, 1)));
    CHECK(fixture.settle().empty());
    // The engine dropped what it kept for this tab, as for a tab that closed.
    static_cast<void>(fixture.engine.step(
        TabClosed { .tab = std::string { DashboardFixture::Tab }, .monoMs = fixture.monoMs() }));
    fixture.dashboard.engineChanged();
    REQUIRE(fixture.engine.transcript(DashboardFixture::Tab, RunningId) == nullptr);

    auto const due = fixture.dashboard.dueActions();
    CHECK(std::ranges::contains(
        due, UiAction { ReadTranscriptAction { .run = std::string { RunningId }, .page = 1 } }));
    fixture.carryOut(
        due, Answers { .transcript = printed(pageJson({ { .kind = "user", .text = "one" } }, 1, 1)) });
    CHECK(fixture.settle().empty());
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
    REQUIRE(fixture.dashboard.selectedRun().empty());
    CHECK(fixture.dashboard.input("t").empty());
    CHECK_FALSE(fixture.dashboard.interaction().chosenTab.has_value());
    CHECK(fixture.settle().empty());
}

TEST_CASE("selecting another run reads its transcript", "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    openTranscript(fixture, RunningId, printed(basicPage()));
    CHECK(fixture.dashboard.select(FinishedId).empty());
    CHECK(fixture.dashboard.interaction().place == Place::Fleet);
    CHECK(fixture.dashboard.selectedRun() == FinishedId);
    auto const due = fixture.dashboard.dueActions();
    CHECK(std::ranges::contains(
        due, UiAction { ReadTranscriptAction { .run = std::string { FinishedId }, .page = 1 } }));
}

// }}}
// {{{ running a finished run again

TEST_CASE("enter on a Finished row opens the launcher on its item and task",
          "[orchestrator][view][transcript]")
{
    auto fixture = DashboardFixture { transcriptWorld(), Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(FinishedId).empty());
    CHECK(fixture.dashboard.input("\r").empty());
    REQUIRE(fixture.dashboard.launcher().has_value());
    auto const& form = fixture.dashboard.launcher()->form;
    REQUIRE(form.has_value());
    CHECK(form->repo == "contour");
    CHECK(form->item == ItemRef { ChangeRequestRef { 2369 } });
    CHECK(form->task == 0); // fix_ci
    CHECK_FALSE(form->session.empty());
    // Nothing starts before the launcher is confirmed.
    CHECK(fixture.screen().contains("New agent"));
}

TEST_CASE("running again offers the item's default task when the run's task is gone",
          "[orchestrator][view][transcript]")
{
    constexpr auto RetiredId = "pr-2346-20261113-140211"sv;
    auto world = transcriptWorld();
    world.stored.push_back(finishedRun(RetiredId, ChangeRequestRef { 2346 }, "retired_task"));
    auto fixture = DashboardFixture { world, Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(RetiredId).empty());
    CHECK(fixture.dashboard.input("\r").empty());
    REQUIRE(fixture.dashboard.launcher().has_value());
    CHECK(fixture.dashboard.launcher()->form->item == ItemRef { ChangeRequestRef { 2346 } });
    CHECK(fixture.dashboard.launcher()->form->task == 0); // fix_ci, the first task for a pull request
}

TEST_CASE("running again an item with an active run shows the refusal", "[orchestrator][view][transcript]")
{
    constexpr auto EarlierId = "pr-2375-20261112-100000"sv;
    auto world = transcriptWorld();
    world.stored.push_back(finishedRun(EarlierId, ChangeRequestRef { 2375 }));
    auto fixture = DashboardFixture { world, Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(EarlierId).empty());
    CHECK(fixture.dashboard.input("\r").empty());
    REQUIRE(fixture.dashboard.launcher().has_value());
    CHECK(fixture.screen().contains("refused"));
    // Enter confirms nothing the preview refuses.
    CHECK(fixture.dashboard.input("\r").empty());
}

TEST_CASE("running again an item no longer listed says so", "[orchestrator][view][transcript]")
{
    auto world = transcriptWorld();
    world.prs = { "github/prs-empty.json" };
    auto fixture = DashboardFixture { world, Medium };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.select(FinishedId).empty());
    CHECK(fixture.dashboard.input("\r").empty());
    CHECK_FALSE(fixture.dashboard.launcher().has_value());
    CHECK(fixture.dashboard.interaction().notice == "#2369 is not listed any more");
}

// }}}
