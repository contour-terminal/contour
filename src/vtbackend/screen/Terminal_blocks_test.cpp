// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/Utils.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::test;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

namespace
{

/// A wall clock that counts its reads, so a test can pin "once per PTY batch" rather than infer it.
class CountingWallClock final: public core::platform::IWallClock
{
  public:
    [[nodiscard]] std::chrono::system_clock::time_point now() const noexcept override
    {
        ++_reads;
        return std::chrono::system_clock::time_point {};
    }

    [[nodiscard]] int reads() const noexcept { return _reads; }

  private:
    mutable int _reads = 0;
};

/// The birth stamp of the primary screen's row @p line.
[[nodiscard]] uint32_t bornAtRow(MockTerm<>& mc, int line)
{
    return mc.terminal.primaryScreen().grid().lineAt(LineOffset(line)).bornAt();
}

/// What MockTerm's wall clock reads when the terminal is built: the epoch row stamps count from.
[[nodiscard]] std::chrono::system_clock::time_point sessionEpoch()
{
    return std::chrono::system_clock::time_point {};
}

/// A sequence that resets the cursor's row in place, the way a progress line is finalised or a prompt
/// redrawn: the reset clears what the row carried, while the cursor stays on it.
struct InPlaceReset
{
    std::string_view name;     ///< What the sequence is, for the test's report.
    std::string_view sequence; ///< The sequence, sent with the cursor at column 0.
};

/// Every sequence that resets the cursor's row in place.
constexpr auto InPlaceResets = std::array {
    InPlaceReset { .name = "EL 0 from column 0", .sequence = "\033[K" },
    InPlaceReset { .name = "EL 2", .sequence = "\033[2K" },
    InPlaceReset { .name = "ED 0 from column 0", .sequence = "\033[J" },
    InPlaceReset { .name = "DECSEL 2", .sequence = "\033[?2K" },
    InPlaceReset { .name = "DECSED 2", .sequence = "\033[?2J" },
    InPlaceReset { .name = "DECALN", .sequence = "\033#8" },
};

// The terminal borrows its clocks for its whole life, so a temporary one -- which would dangle -- is
// refused at compile time: the steady clock by SteadyClockRef, the wall clock by WallClockRef. Each
// refusal sits beside the acceptance it must not have taken with it.
static_assert(std::is_constructible_v<SteadyClockRef, core::platform::SteadyClock&>);
static_assert(!std::is_constructible_v<SteadyClockRef, core::platform::SteadyClock>);
static_assert(std::is_constructible_v<SteadyClockRef, core::platform::ManualClock&>);
static_assert(!std::is_constructible_v<SteadyClockRef, core::platform::ManualClock>);

/// Whether `TerminalClocks { .steady = ..., .wall = ..., .localTime = ... }` compiles for clocks of these
/// value categories.
template <typename Steady, typename Wall>
concept DesignatesClocks = requires {
    TerminalClocks { .steady = std::declval<Steady>(),
                     .wall = std::declval<Wall>(),
                     .localTime = &systemLocalTime };
};

// ...and through the aggregate, in the designated form every call site writes.
static_assert(DesignatesClocks<core::platform::ManualClock&, core::platform::ManualWallClock&>);
static_assert(!DesignatesClocks<core::platform::ManualClock, core::platform::ManualWallClock&>);
static_assert(!DesignatesClocks<core::platform::ManualClock&, core::platform::ManualWallClock>);

// The borrowed steady clock still goes wherever an `IClock const&` is expected, and is handed on as it is
// to the command block store, which holds the same borrow.
static_assert(std::is_convertible_v<SteadyClockRef const&, core::platform::IClock const&>);
static_assert(std::is_constructible_v<CommandBlockStore,
                                      CommandBlockStoreLimits,
                                      SteadyClockRef const&,
                                      core::platform::WallClockRef,
                                      CommandBlockChangeListener>);

} // namespace

// {{{ row birth times

TEST_CASE("Terminal.blocks.aRowIsBornWhenTheCursorFirstReachesIt", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };

    mc.wallClock.advance(5s);
    mc.writeToScreen("one\r\n"); // rows 0 and 1 are reached in this batch
    mc.wallClock.advance(10s);
    mc.writeToScreen("two\r\nthree"); // row 1 was reached already; row 2 is new

    CHECK(bornAtRow(mc, 0) == 6); // seconds since the session started, plus one
    CHECK(bornAtRow(mc, 1) == 6);
    CHECK(bornAtRow(mc, 2) == 16);
    CHECK(bornAtRow(mc, 3) == 0); // never reached

    auto const& grid = mc.terminal.primaryScreen().grid();
    CHECK(mc.terminal.lineBirthTime(grid.lineAt(LineOffset(2))) == sessionEpoch() + 15s);
    CHECK_FALSE(mc.terminal.lineBirthTime(grid.lineAt(LineOffset(3))).has_value());
}

TEST_CASE("Terminal.blocks.returningToARowKeepsItsFirstTime", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.wallClock.advance(5s);
    mc.writeToScreen("a\r\nb");
    mc.wallClock.advance(60s);
    mc.writeToScreen("\033[Hx"); // back to row 0, and write over it

    CHECK(bornAtRow(mc, 0) == 6);
}

TEST_CASE("Terminal.blocks.aRecycledRowIsBornAgain", "[terminal][semanticblocks]")
{
    // With no scrollback, the row that scrolls off the top is reset and reused at the bottom: a new line,
    // whose time is the time the cursor reached it in its new life.
    auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(10) }, LineCount(0) };
    mc.wallClock.advance(5s);
    mc.writeToScreen("a\r\nb");
    mc.wallClock.advance(20s);
    mc.writeToScreen("\r\nc");

    CHECK(bornAtRow(mc, 0) == 6);  // "b", scrolled up from row 1
    CHECK(bornAtRow(mc, 1) == 26); // "c", on the recycled row
}

TEST_CASE("Terminal.blocks.theWallClockIsReadOncePerBatch", "[terminal][semanticblocks]")
{
    // No clock read per line: a batch that crosses five rows reads the wall clock once.
    auto events = Terminal::NullEvents {};
    auto steady = core::platform::ManualClock {};
    auto wall = CountingWallClock {};
    auto const pageSize = PageSize { LineCount(10), ColumnCount(20) };
    auto settings = Settings {};
    settings.pageSize = pageSize;
    auto terminal =
        Terminal { events,
                   core::defaultEnvironment(),
                   core::platform::NativeFileSystem::instance(),
                   std::make_unique<vtpty::MockPty>(pageSize),
                   std::move(settings),
                   std::nullopt,
                   TerminalClocks { .steady = steady, .wall = wall, .localTime = &systemLocalTime },
                   randomSemanticBlockTokens() };
    auto const atBirth = wall.reads();

    terminal.writeToScreen("1\r\n2\r\n3\r\n4\r\n5\r\n");
    CHECK(wall.reads() - atBirth == 1);

    terminal.writeToScreen("6\r\n");
    CHECK(wall.reads() - atBirth == 2);
}

TEST_CASE("Terminal.blocks.alternateScreenRowsAreNeverStamped", "[terminal][semanticblocks]")
{
    // The gutter shows nothing on the alternate screen, so its rows carry no time -- and a full-screen
    // application repainting every row must not dirty them for a stamp nobody reads.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.wallClock.advance(5s);
    mc.writeToScreen("\033[?1049h");
    mc.writeToScreen("x\r\ny\r\nz");

    auto const& alternate = mc.terminal.alternateScreen().grid();
    for (auto const line: std::views::iota(0, 3))
        CHECK(alternate.lineAt(LineOffset(line)).bornAt() == 0);
}

TEST_CASE("Terminal.blocks.aStampPastTheHorizonSaturatesAndShowsNoTime", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.wallClock.advance(std::chrono::seconds { Line::MaxBornAt } + 10s);
    mc.writeToScreen("x");

    auto const& line = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));
    CHECK(line.bornAt() == Line::MaxBornAt);
    CHECK_FALSE(mc.terminal.lineBirthTime(line).has_value());
}

TEST_CASE("Terminal.blocks.aWallClockStepBackwardsStampsTheSessionStart", "[terminal][semanticblocks]")
{
    // NTP may step the wall clock back. A row cannot be born before its session was.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.wallClock.setNow(sessionEpoch() - 1h);
    mc.writeToScreen("x");

    auto const& line = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));
    CHECK(line.bornAt() == 1);
    CHECK(mc.terminal.lineBirthTime(line) == sessionEpoch());
}

TEST_CASE("Terminal.blocks.aRowsTimeIsTheSecondItWasBornIn", "[terminal][semanticblocks]")
{
    // A session may begin part-way through a second. Counted from that unrounded start, a row born 0.6 s
    // later -- already in the next second -- would read as born in the first.
    auto events = Terminal::NullEvents {};
    auto steady = core::platform::ManualClock {};
    auto const zero = std::chrono::system_clock::time_point {};
    auto wall = core::platform::ManualWallClock { zero + 10s + 900ms };
    auto const pageSize = PageSize { LineCount(5), ColumnCount(20) };
    auto settings = Settings {};
    settings.pageSize = pageSize;
    auto terminal =
        Terminal { events,
                   core::defaultEnvironment(),
                   core::platform::NativeFileSystem::instance(),
                   std::make_unique<vtpty::MockPty>(pageSize),
                   std::move(settings),
                   std::nullopt,
                   TerminalClocks { .steady = steady, .wall = wall, .localTime = &systemLocalTime },
                   randomSemanticBlockTokens() };

    wall.advance(600ms);
    terminal.writeToScreen("x");

    auto const& line = terminal.primaryScreen().grid().lineAt(LineOffset(0));
    CHECK(terminal.lineBirthTime(line) == zero + 11s);
}

TEST_CASE("Terminal.blocks.onlyThePrimaryPageIsStamped", "[terminal][semanticblocks]")
{
    // The stamp belongs to the primary screen alone, not to every screen the way an OSC 3008 context is
    // fanned out: a DEC page other than the first and the host-writable status line show no time either,
    // so their rows stay unstamped and are never dirtied for one.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mc.wallClock.advance(5s);

    SECTION("a DEC page other than the first")
    {
        mc.writeToScreen("\033[2 P"); // PPA 2
        mc.writeToScreen("x\r\ny");

        auto const& page = mc.terminal.pageAt(PageIndex(1));
        REQUIRE(page.renderMainPageText().starts_with("x")); // the rows were reached, on this page
        for (auto const line: std::views::iota(0, 3))
            CHECK(page.grid().lineAt(LineOffset(line)).bornAt() == 0);
    }

    SECTION("the host-writable status line")
    {
        mc.writeToScreen("\033[1$}"); // DECSASD: to the status line
        mc.writeToScreen("x");

        auto const& statusLine = mc.terminal.hostWritableStatusLineDisplay();
        REQUIRE(statusLine.renderMainPageText().starts_with("x")); // the row was reached, on this screen
        CHECK(statusLine.grid().lineAt(LineOffset(0)).bornAt() == 0);
    }
}

TEST_CASE("Terminal.blocks.aRowReachedBetweenBatchesIsStampedByTheNextBatch", "[terminal][semanticblocks]")
{
    // A reset the GUI asks for (ClearHistoryAndReset) runs between batches and puts the cursor on a fresh
    // row 0. That row must not take the time of the batch before -- possibly hours old -- because a stamp
    // is never rewritten: it waits for the next batch, the first one it is seen in.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.wallClock.advance(5s);
    mc.writeToScreen("one\r\ntwo");
    mc.wallClock.advance(60s);
    core::locked(mc.terminal, [&]() { mc.terminal.hardReset(); });

    CHECK(bornAtRow(mc, 0) == 0);

    mc.wallClock.advance(60s);
    mc.writeToScreen("x");

    CHECK(bornAtRow(mc, 0) == 126);
}

TEST_CASE("Terminal.blocks.aRowResetUnderTheCursorIsBornAgainInThatBatch", "[terminal][semanticblocks]")
{
    // A progress line is finalised, and a prompt redrawn, by resetting the row the cursor is on, then
    // rewriting it -- and the cursor may leave it within the same batch. The reset clears the row's time,
    // as a reset must; the row is born again in that batch, or it would never carry a time at all.
    for (auto const& reset: InPlaceResets)
    {
        INFO(reset.name);
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
        mc.wallClock.advance(5s);
        mc.writeToScreen("50%");
        mc.wallClock.advance(10s);
        mc.writeToScreen(std::format("\r{}done\r\n", reset.sequence));

        CHECK(bornAtRow(mc, 0) == 16);
    }
}

// }}}

// {{{ OSC 133 feeds the store

namespace
{

/// The id of the block the primary screen's row @p line belongs to.
[[nodiscard]] CommandBlockId blockOfRow(MockTerm<>& mc, int line)
{
    return mc.terminal.primaryScreen().grid().lineAt(LineOffset(line)).blockId();
}

} // namespace

TEST_CASE("Terminal.blocks.aPromptStartMintsARecordAndStampsItsRow", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("\033]133;A\033\\$ ");

    REQUIRE(mc.terminal.commandBlocks().size() == 1);
    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->state == CommandBlockState::Prompting);
    CHECK(blockOfRow(mc, 0) == record->id);
    CHECK(mc.terminal.commandBlockAt(LineOffset(0)) == record);
    CHECK(record->headStableId == mc.terminal.primaryScreen().grid().stableLineIdOf(LineOffset(0)));
}

TEST_CASE("Terminal.blocks.everyRowOfABlockCarriesItsIdAndTheNextPromptStartsANewOne",
          "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) } };
    runCommand(mc, "ls", { "a", "b" });
    REQUIRE(mc.terminal.commandBlocks().lastFinished() != nullptr);
    auto const first = mc.terminal.commandBlocks().lastFinished()->id;
    mc.writeToScreen("\033]133;A\033\\$ ");
    auto const second = mc.terminal.commandBlocks().currentId();

    CHECK(blockOfRow(mc, 0) == first); // "$ ls"
    CHECK(blockOfRow(mc, 1) == first); // "a"
    CHECK(blockOfRow(mc, 2) == first); // "b"
    // The ;D and the next ;A land on the same row: last writer wins, as for OSC 3008 contexts.
    CHECK(blockOfRow(mc, 3) == second);
    CHECK(second != first);
}

TEST_CASE("Terminal.blocks.aRowTheCursorMerelyRevisitsKeepsItsBlock", "[terminal][semanticblocks]")
{
    // Reaching a row is not writing it. A row takes the running block when the cursor first reaches it;
    // moving back onto it later -- even printing over it -- leaves it in the block it was born in.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) } };
    runCommand(mc, "ls", { "a", "b", "c" }); // rows 0..3
    auto const first = mc.terminal.commandBlocks().lastFinished()->id;
    typeAtPrompt(mc, "prog"); // row 4; the cursor is on row 5
    mc.writeToScreen("\033]133;C\033\\");
    auto const second = mc.terminal.commandBlocks().currentId();
    REQUIRE(second != first);

    SECTION("the cursor homes onto an older prompt")
    {
        mc.writeToScreen("\033[H");
        CHECK(blockOfRow(mc, 0) == first);
    }

    SECTION("a program steps back up over earlier output and prints over it")
    {
        mc.writeToScreen("\033[4Ax");
        CHECK(blockOfRow(mc, 1) == first);
    }

    CHECK(blockOfRow(mc, 4) == second);
    CHECK(blockOfRow(mc, 5) == second);
}

TEST_CASE("Terminal.blocks.aPromptOnAWrappedOutputLineClaimsTheWholeLogicalLine",
          "[terminal][semanticblocks]")
{
    // Output wider than the page with no trailing newline: precmd's ;D and ;A land on the LAST physical
    // row of it. ;A marks the logical line's head, and reflow hands every chunk the head's id -- so the new
    // block owns the whole logical line, or the next resize would hand its prompt back to the previous one.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(10) } };
    typeAtPrompt(mc, "wide");
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen(std::string(25, 'x')); // rows 1..3
    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

    auto const second = mc.terminal.commandBlocks().currentId();
    CHECK(blockOfRow(mc, 1) == second);
    CHECK(blockOfRow(mc, 2) == second);
    CHECK(blockOfRow(mc, 3) == second);
    REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
    CHECK(mc.terminal.commandBlocks().current()->headStableId
          == mc.terminal.primaryScreen().grid().stableLineIdOf(LineOffset(1)));
}

TEST_CASE("Terminal.blocks.aTransitionHandingOutWhatItsLineCarriesLooksAtOneRow",
          "[terminal][semanticblocks]")
{
    // A single-line `cat` of a megabyte, and the prompt drawn at its end. The ;A hands the whole logical line
    // to its new block, every row once. The ;C after it hands out that same block, which the line carries
    // already: it looks at the cursor's row, not at the whole line -- most of the history -- again.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(10) }, LineCount(100) };
    auto const& screen = mc.terminal.primaryScreen();
    typeAtPrompt(mc, "cat");
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen(std::string(395, 'x')); // one logical line of 40 rows, most of them in the history

    auto const beforePrompt = screen.restampWork();
    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\");
    CHECK(screen.restampWork() - beforePrompt == 40);

    auto const beforeStart = screen.restampWork();
    mc.writeToScreen("$ \033]133;C\033\\");
    CHECK(screen.restampWork() - beforeStart == 1);

    // And the whole line is the new block's, as the widening that rejoins it would make it.
    auto const& grid = screen.grid();
    auto const id = mc.terminal.commandBlocks().currentId();
    auto const rows =
        std::views::iota(-unbox<int>(grid.historyLineCount()), unbox<int>(mc.terminal.pageSize().lines));
    CHECK(std::ranges::count_if(
              rows, [&](int row) { return grid.lineAt(LineOffset::cast_from(row)).blockId() == id; })
          == 40);
}

TEST_CASE("Terminal.blocks.aRowResetUnderTheCursorStaysInItsBlock", "[terminal][semanticblocks]")
{
    // `\r\e[K` finalising a progress line inside a running command: the reset clears the row's block, as a
    // reset must, yet the command is still writing that row -- and the cursor never leaves it, so nothing
    // else would hand it back. Rows a reset blanks WITHOUT the cursor on them stay cleared: nothing writes
    // them, and the cursor stamps them when it reaches them.
    for (auto const& reset: InPlaceResets)
    {
        INFO(reset.name);
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
        typeAtPrompt(mc, "make");
        mc.writeToScreen("\033]133;C\033\\50%");
        auto const running = mc.terminal.commandBlocks().currentId();
        REQUIRE(!!running);

        mc.writeToScreen(std::format("\r{}", reset.sequence));
        // Row 1, the progress line -- or row 0 under DECALN, which homes the cursor.
        auto const resetRow = unbox<int>(mc.terminal.primaryScreen().cursor().position.line);
        mc.writeToScreen("done\r\n");

        CHECK(blockOfRow(mc, resetRow) == running);
        CHECK(blockOfRow(mc, resetRow + 1) == running); // reached by the newline
    }
}

TEST_CASE("Terminal.blocks.theCommandLineComesFromTheShellWhenItSaysSo", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    typeAtPrompt(mc, "ls -la");
    mc.writeToScreen("\033]133;C;cmdline_url=ls%20-la%20%2Ftmp\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == "ls -la /tmp"); // what the shell said, not what the grid shows
    CHECK(record->commandLineSource == CommandLineSource::Reported);
}

TEST_CASE("Terminal.blocks.theCommandLineComesFromKittysCmdline", "[terminal][semanticblocks]")
{
    // kitty's integrations report the command line as `cmdline=`, quoted by `printf %q` -- a ';' in it is the
    // command's own.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    auto const commandAfter = [&](std::string_view parameters) {
        typeAtPrompt(mc, "ls -la");
        mc.writeToScreen(std::format("\033]133;C{}\033\\", parameters));
        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        auto ran = *record;
        mc.writeToScreen("\033]133;D;0\033\\");
        return ran;
    };

    SECTION("quoted")
    {
        auto const record = commandAfter(R"(;cmdline=ls\ -la\ /tmp\;\ pwd)");
        CHECK(record.commandLine.raw() == "ls -la /tmp; pwd");
        CHECK(record.commandLineSource == CommandLineSource::Reported);
    }
    SECTION("empty, it reports nothing and the typed command is recovered")
    {
        for (auto const parameters: { ";cmdline="sv, ";cmdline=''"sv })
        {
            INFO(parameters);
            auto const record = commandAfter(parameters);
            CHECK(record.commandLine.raw() == "ls -la");
            CHECK(record.commandLineSource == CommandLineSource::Recovered);
        }
    }
    SECTION("malformed, likewise")
    {
        auto const record = commandAfter(";cmdline='ls");
        CHECK(record.commandLine.raw() == "ls -la");
        CHECK(record.commandLineSource == CommandLineSource::Recovered);
    }
}

TEST_CASE("Terminal.blocks.theCommandLineIsRecoveredFromTheGridAfterPromptEnd", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) } };

    SECTION("a one-line command")
    {
        typeAtPrompt(mc, "ls -la");
        mc.writeToScreen("\033]133;C\033\\");
        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        CHECK(record->commandLine.raw() == "ls -la");
        CHECK(record->commandLineSource == CommandLineSource::Recovered);
    }

    SECTION("a command that wrapped is read back whole")
    {
        typeAtPrompt(mc, "0123456789abc"); // "$ " + 13 columns on a 10-column page
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "0123456789abc");
    }

    SECTION("a prompt that wraps before the input begins")
    {
        // "user@host:~$ " is 13 columns on a 10-column page: the input begins on the prompt's second row.
        mc.writeToScreen("\033]133;A\033\\user@host:~$ \033]133;B\033\\ls\r\n");
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "ls");
    }

    SECTION("a continued command keeps its line break")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\echo a \\\r\n> b\r\n");
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "echo a \\\n> b");
    }

    SECTION("a row of the shell's own below the input, blank but for a no-break space, is not input")
    {
        // endo keeps a spacer row below its input and starts it with U+00A0, so that the row is never blank;
        // on Enter it steps the cursor past that row before it says ;C.
        mc.writeToScreen("\033]133;A\033\\$ \r\n\xC2\xA0\033[1A\r\033[2C\033]133;B\033\\ls");
        mc.writeToScreen("\033[1B\r\r\n\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "ls");
    }
}

TEST_CASE("Terminal.blocks.aPromptRedrawnInPlaceStillSaysWhereItsInputBegins", "[terminal][semanticblocks]")
{
    // A shell repaints its prompt line with `\r\e[K` and prints the prompt again without a second ;B. The
    // erase keeps the row's PromptEnd; were it to drop the column that belongs to it, the command recovered
    // at ;C would read the prompt as typed input.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\l");
    mc.writeToScreen("\r\033[K$ ls\r\n");
    mc.writeToScreen("\033]133;C\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == "ls");
    CHECK(record->commandLineSource == CommandLineSource::Recovered);
}

TEST_CASE("Terminal.blocks.aNewPromptStartingOnAnOlderOnesRowRecoversOnlyItsOwnInput",
          "[terminal][semanticblocks]")
{
    // An erase keeps a row's PromptEnd with its column, so a row can still carry an older prompt's input
    // border when a new prompt starts on it. With the new prompt spanning two lines and its own ;B on the
    // second, the stale border on the first was read as where the input begins.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\"); // a prompt, never answered
    mc.writeToScreen("\r\033[K");                           // erased, to be drawn again on two lines
    mc.writeToScreen("\033]133;A\033\\user@host ~\r\n$ \033]133;B\033\\ls\r\n");
    mc.writeToScreen("\033]133;C\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == "ls");
}

TEST_CASE("Terminal.blocks.aRightPromptBesideTheCommandIsNotRecovered", "[terminal][semanticblocks]")
{
    // zsh draws RPROMPT at the right margin after the prompt and leaves it there once Enter is pressed
    // (unless TRANSIENT_RPROMPT is set). Read from ;B to the cursor, it became part of the command.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    auto const runWithRightPrompt = [&](std::string_view typed, int rightPromptColumn) {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
        mc.writeToScreen(
            std::format("\0337\033[{}G~/src 10:42\0338", rightPromptColumn)); // DECSC, CHA, DECRC
        mc.writeToScreen(typed);
        mc.writeToScreen("\r\n\033]133;C\033\\");
        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        return std::pair { record->commandLine.raw(), record->commandLineSource };
    };

    SECTION("one column short of the margin, zsh's default indent")
    {
        CHECK(runWithRightPrompt("ls -la", 29).first == "ls -la");
    }
    SECTION("reaching the margin")
    {
        CHECK(runWithRightPrompt("ls -la", 30).first == "ls -la");
    }
    SECTION("with nothing typed")
    {
        CHECK(runWithRightPrompt("", 30).second == CommandLineSource::None);
    }
    SECTION("blanks of the command's own are kept, beside a right prompt")
    {
        CHECK(runWithRightPrompt("echo a    b", 29).first == "echo a    b");
    }
    SECTION("blanks of the command's own are kept, with no right prompt")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\echo a    b\r\n\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "echo a    b");
    }
}

TEST_CASE("Terminal.blocks.aRightPromptTheShellMarksIsNotRecovered", "[terminal][semanticblocks]")
{
    // OSC 133;P;k=r says where a right prompt begins (Nushell sends it). One blank apart from the command, it
    // is too close to pass for a right prompt by its looks alone.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    auto const drawRightPrompt = [&](int column, std::string_view mark) {
        mc.writeToScreen(std::format("\0337\033[{}G{}~/src 10:42\0338", column, mark)); // DECSC, CHA, DECRC
    };

    SECTION("marked")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
        auto const block = mc.terminal.commandBlocks().currentId();
        drawRightPrompt(10, "\033]133;P;k=r\033\\");
        CHECK(mc.terminal.commandBlocks().currentId() == block); // a right prompt starts no block
        mc.writeToScreen("ls -la\r\n\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "ls -la");
    }

    SECTION("not marked, so the heuristic cannot tell it from the command")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
        drawRightPrompt(10, "");
        mc.writeToScreen("ls -la\r\n\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "ls -la ~/src 10:42");
    }

    SECTION("marked on one line, the heuristic still serves another")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
        drawRightPrompt(10, "\033]133;P;k=r\033\\");
        mc.writeToScreen("echo \\\r\n> \033]133;B\033\\");
        drawRightPrompt(9, ""); // two blanks apart, reaching the margin
        mc.writeToScreen("b\r\n\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "echo \\\nb");
    }

    SECTION("written over by the command once it reached it")
    {
        // zsh takes RPROMPT away once the command reaches it: the command is drawn over it, the rest erased.
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
        drawRightPrompt(10, "\033]133;P;k=r\033\\");
        mc.writeToScreen("echo abcdefghij\033[K\r\n\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "echo abcdefghij");
    }

    SECTION("marked by an older prompt, it is not this prompt's")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
        drawRightPrompt(10, "\033]133;P;k=r\033\\");
        mc.writeToScreen("\r\033[K\033]133;A\033\\$ \033]133;B\033\\echo a  b c\r\n\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "echo a  b c");
    }
}

TEST_CASE("Terminal.blocks.aCommandWhoseInputEndsWithItsLineIsRecoveredFromThatLine",
          "[terminal][semanticblocks]")
{
    // OSC 133;I is ;B whose input ends with its line: what follows it before the output start is no part of
    // the command.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };

    SECTION("with I")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;I\033\\ls\r\nfile\r\n\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "ls");
    }

    SECTION("with B")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\ls\r\nfile\r\n\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "ls\nfile");
    }

    SECTION("with I, then B again on the same line")
    {
        // The prompt redrawn, its end marked with ;B this time: the input no longer ends with the line.
        mc.writeToScreen("\033]133;A\033\\$ \033]133;I\033\\\r\033[K$ \033]133;B\033\\ls\r\nfile\r\n");
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "ls\nfile");
    }
}

TEST_CASE("Terminal.blocks.aMarkedContinuationPromptIsNotRecovered", "[terminal][semanticblocks]")
{
    // A shell that marks its continuation prompt the way it marks its prompt -- ;B after the `> ` -- says
    // where each further line of the command begins, so only what follows it was typed. (An unmarked one
    // cannot be told from the command: a continued command keeps its line break, above.)
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\echo a \\\r\n");
    mc.writeToScreen("> \033]133;B\033\\b \\\r\n");
    mc.writeToScreen("> \033]133;B\033\\c\r\n");
    mc.writeToScreen("\033]133;C\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == "echo a \\\nb \\\nc");
}

TEST_CASE("Terminal.blocks.aContinuationPromptAnnouncedAsSuchIsNotRecovered", "[terminal][semanticblocks]")
{
    // The same command under continuation prompts the shell announces as such -- ;A;k=s before the `> `, as
    // kitty's integrations and Nushell do: each continues the block being typed, so the command is one block,
    // read from every line's own ;B.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    mc.writeToScreen(typedAtPrompt("echo a \\"));
    auto const block = mc.terminal.commandBlocks().currentId();
    mc.writeToScreen("\033]133;A;k=s\033\\> \033]133;B\033\\b \\\r\n");
    mc.writeToScreen("\033]133;A;k=s\033\\> \033]133;B\033\\c\r\n");
    mc.writeToScreen("\033]133;C\033\\");

    CHECK(mc.terminal.commandBlocks().size() == 1);
    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->id == block);
    CHECK(record->commandLine.raw() == "echo a \\\nb \\\nc");
    CHECK(record->commandLineSource == CommandLineSource::Recovered);
}

TEST_CASE("Terminal.blocks.aBlankLineInsideACommandIsKept", "[terminal][semanticblocks]")
{
    // A here-document with an empty line, typed under an empty continuation prompt: trimming the text before
    // each line break also took the line break before it, so the empty line vanished.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) } };
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\cat <<E\r\na\r\n\r\nb\r\nE\r\n");
    mc.writeToScreen("\033]133;C\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == "cat <<E\na\n\nb\nE");
}

TEST_CASE("Terminal.blocks.theCommandAfterOneThatPrintedNothingIsRecovered", "[terminal][semanticblocks]")
{
    // `cd` prints nothing, so its ;C's output start, its ;D and the next prompt's ;A share a line. That
    // output start is `cd`'s; read as the new prompt's, it put the prompt inside a command's output and lost
    // `ls`.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    typeAtPrompt(mc, "cd /tmp");
    mc.writeToScreen("\033]133;C\033\\\033]133;D;0\033\\");
    typeAtPrompt(mc, "ls");
    mc.writeToScreen("\033]133;C\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == "ls");
    CHECK(record->commandLineSource == CommandLineSource::Recovered);

    SECTION("a command that printed something is unaffected")
    {
        mc.writeToScreen("a-file\r\n\033]133;D;0\033\\");
        typeAtPrompt(mc, "pwd");
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->commandLine.raw() == "pwd");
    }
}

TEST_CASE("Terminal.blocks.thePromptAfterACommandWithNoEndMarkIsAPrompt", "[terminal][semanticblocks]")
{
    // A shell that sends ;C but never ;D. When the command printed no line, the next prompt's ;A lands on the
    // line the command's output started on. That prompt ends the command there, as a ;D would have, so the
    // line reads as the prompt it now is: kitty reads a line by the last mark on it.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    auto const recoverLs = [&] {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\ls");
        CHECK(mc.terminal.livePromptSpan().has_value());
        mc.writeToScreen("\r\n\033]133;C\033\\");
        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        CHECK(record->commandLine.raw() == "ls");
    };

    SECTION("a command that printed nothing")
    {
        typeAtPrompt(mc, "true");
        mc.writeToScreen("\033]133;C\033\\");
        recoverLs();
    }

    SECTION("a command that printed part of a line")
    {
        typeAtPrompt(mc, "printf hi");
        mc.writeToScreen("\033]133;C\033\\hi");
        recoverLs();
    }

    SECTION("a command a ;D ended keeps the end it was given")
    {
        // Something drawn after the ;D on the same line, before the ;A, is not the command's output.
        typeAtPrompt(mc, "printf hi");
        mc.writeToScreen("\033]133;C\033\\hi\033]133;D;0\033\\ [0] ");
        recoverLs();
        CHECK(mc.terminal
                  .commandBlockText(
                      CommandBlockTarget::Last, CommandBlockPart::Output, CaptureRendition::PlainText)
                  .value_or("<none>")
              == "hi");
    }

    SECTION("an output start after the prompt start on one line is still a running command")
    {
        // The other order: ;C before the newline, as a key handler on Enter would send it. The last mark on
        // the line is the output start.
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\make\033]133;C\033\\\r\nbuilding");
        auto const span = mc.terminal.livePromptSpan();
        REQUIRE_FALSE(span.has_value());
        CHECK(span.error() == PromptRegionError::InCommandOutput);
    }
}

TEST_CASE("Terminal.blocks.aBlockWhoseHeadLeftTheHistoryKeepsTheRestOfItsPrompt",
          "[terminal][semanticblocks]")
{
    // Four rows of page and two of history: the first line of a two-line prompt scrolls out of it, the line
    // the command was typed on does not.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(2) };
    mc.writeToScreen(
        "\033]133;A\033\\~/src\r\n$ \033]133;B\033\\make\r\n\033]133;C\033\\1\r\n2\r\n3\r\n4\r\n");
    mc.writeToScreen(commandEnd(0));
    mc.writeToScreen(PromptStart);
    REQUIRE(mc.terminal.primaryScreen().grid().lineTextTrimmed(LineOffset(-2)) == "$ make");

    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Prompt) == "$ make");
    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::PromptAndOutput) == "$ make\n1\n2\n3\n4");
    CHECK(textOf(mc, CommandBlockTarget::Last, CommandBlockPart::Output) == "1\n2\n3\n4");
}

TEST_CASE("Terminal.blocks.aBlockAtItsPromptHasNoOutputOnceItsHeadLeftTheHistory",
          "[terminal][semanticblocks]")
{
    // A prompt taller than the page and its history: its head scrolls out while no command has started.
    // What is left of it is still prompt, not output.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(2) };
    mc.writeToScreen("\033]133;A\033\\p1\r\np2\r\np3\r\np4\r\np5\r\np6\r\n$ ");
    REQUIRE(mc.terminal.primaryScreen().grid().lineTextTrimmed(LineOffset(-2)) == "p2");

    CHECK(mc.terminal.commandBlockRows(CommandBlockTarget::Cursor, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(mc.terminal
              .commandBlockText(
                  CommandBlockTarget::Cursor, CommandBlockPart::Prompt, CaptureRendition::PlainText)
              .value_or("<none>")
          == "p2\np3\np4\np5\np6\n$");
}

TEST_CASE("Terminal.blocks.aHeadTheHistoryLetsGoOfChangesItsRecordOnce", "[terminal][semanticblocks]")
{
    // A replica is told whether a record's head row is gone, so its going is a change of the record, and the
    // record is stamped with it: what a sender walking the change stamps sends again. Output alone, before
    // and after, is no change of the record.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(2) };
    startCommand(mc, "make");
    auto const revision = [&] {
        return mc.terminal.commandBlocks().contentRevision();
    };
    auto const stampOfMake = [&] {
        return mc.terminal.commandBlocks().stampedRecords().front().changeStamp;
    };
    auto const headEvicted = [&] {
        return isHeadEvicted(mc.terminal.primaryScreen().grid(),
                             mc.terminal.commandBlocks().records().front());
    };
    auto const started = revision();

    mc.writeToScreen("1\r\n");
    REQUIRE_FALSE(headEvicted());
    CHECK(revision() == started);

    printLines(mc, "out", 6);
    REQUIRE(headEvicted());
    CHECK(revision() > started);
    CHECK(stampOfMake() == revision());

    auto const evicted = revision();
    printLines(mc, "more", 6);
    CHECK(revision() == evicted);
}

TEST_CASE("Terminal.blocks.anEraseThatLeavesNothingOfABlockChangesItsRecordOnce",
          "[terminal][semanticblocks]")
{
    // `printf x` at a shell that sends ;C before running it prints on the prompt's line, and the next prompt,
    // drawn over that line, takes the block's last row. The head is then as gone as one the history let go
    // of, and a replica is told the same, so the record is stamped -- once, also where the history lets a row
    // go in the same look, which puts the head among the rows the floor passed.
    auto const promptDrawnOver = [](MockTerm<>& mc, std::string_view afterThePrompt) {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;C\033\\x\033]133;D;0\033\\");
        auto const id = mc.terminal.commandBlocks().currentId();
        auto const floor = mc.terminal.primaryScreen().grid().stableRangeFloor();
        mc.writeToScreen(std::format("\r\033]133;A\033\\$ {}", afterThePrompt));
        mc.changedCommandBlocks.clear(); // what the new prompt told; the look below tells the rest
        return std::pair { id, floor };
    };
    auto const checkStampedOnce = [](MockTerm<>& mc, CommandBlockId id) {
        auto const& blocks = mc.terminal.commandBlocks();
        auto const& stamped = blocks.stampedRecords().front();
        REQUIRE(stamped.record.id == id);
        REQUIRE(isHeadEvicted(mc.terminal.primaryScreen().grid(), stamped.record));
        CHECK(stamped.changeStamp == blocks.contentRevision());
        // Stamped for a replica, yet no change event: the record says what it said, and the look that found
        // the head gone was a read, on whichever thread made it.
        CHECK(mc.changedCommandBlocks.empty());

        auto const evicted = stamped.changeStamp;
        CHECK(mc.terminal.commandBlocks().stampedRecords().front().changeStamp == evicted);
    };

    SECTION("the history keeps every row")
    {
        auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) }, LineCount(20) };
        auto const [id, floor] = promptDrawnOver(mc, "");
        REQUIRE(mc.terminal.primaryScreen().grid().stableRangeFloor() == floor);
        checkStampedOnce(mc, id);
    }

    SECTION("the history lets a row go in the same look")
    {
        auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(2) };
        printLines(mc, "pad", 6); // the history is full, so the next scroll lets its oldest row go
        auto const [id, floor] = promptDrawnOver(mc, "\r\n");
        REQUIRE(mc.terminal.primaryScreen().grid().stableRangeFloor() > floor);
        checkStampedOnce(mc, id);
    }
}

TEST_CASE("Terminal.blocks.aRecoveredCommandLineIsHeldToTheBound", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(60), ColumnCount(100) } };
    auto const command = std::string(MaxRecordedCommandLineBytes + 500, 'x');
    typeAtPrompt(mc, command);
    mc.writeToScreen("\033]133;C\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == truncatedCommandLine(command));
}

TEST_CASE("Terminal.blocks.withoutPromptEndNothingIsRecovered", "[terminal][semanticblocks]")
{
    // Without ;B the terminal cannot tell the prompt's own text from what was typed into it, and a command
    // line that might read "user@host:~$ ls" is worse than none.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    mc.writeToScreen("\033]133;A\033\\$ ls\r\n\033]133;C\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLineSource == CommandLineSource::None);
    CHECK(record->commandLine.empty());
}

TEST_CASE("Terminal.blocks.anEmptyCmdlineUrlReportsNoCommandLine", "[terminal][semanticblocks]")
{
    // An empty cmdline_url= says nothing about the command, exactly as an empty OSC 3008 cmdline= does, so
    // it is read as though none had been sent rather than recorded as a reported empty command line.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };

    SECTION("after ;B the command line is recovered from the grid")
    {
        typeAtPrompt(mc, "ls -la");
        mc.writeToScreen("\033]133;C;cmdline_url=\033\\");

        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        CHECK(record->commandLine.raw() == "ls -la");
        CHECK(record->commandLineSource == CommandLineSource::Recovered);
    }

    SECTION("without ;B there is none")
    {
        mc.writeToScreen("\033]133;A\033\\$ ls\r\n\033]133;C;cmdline_url=\033\\");

        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        CHECK(record->commandLine.empty());
        CHECK(record->commandLineSource == CommandLineSource::None);
    }
}

TEST_CASE("Terminal.blocks.aHugeCmdlineUrlIsStoredTruncatedAtACodepoint", "[terminal][semanticblocks]")
{
    // 64 KiB of percent-encoded "€" end to end: through the real parser (which itself caps an OSC at
    // Sequence::MaxOscLength) into the store, which keeps a bounded prefix of whole code points.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    auto encoded = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, (64 * 1024 / 9) + 1))
        encoded += "%E2%82%AC";
    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("\033]133;C;cmdline_url=" + encoded + "\033\\");

    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    auto const& line = record->commandLine.raw();
    CHECK(line.size() <= MaxRecordedCommandLineBytes);
    REQUIRE(line.ends_with("\xE2\x80\xA6"));
    auto const kept = std::string_view { line }.substr(0, line.size() - 3);
    CHECK(kept.size() % 3 == 0);
    CHECK(std::ranges::all_of(std::views::iota(size_t { 0 }, kept.size() / 3),
                              [&](size_t i) { return kept.substr(i * 3, 3) == "\xE2\x82\xAC"; }));
}

TEST_CASE("Terminal.blocks.aReportedFinishIsAnnouncedOnce", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    typeAtPrompt(mc, "make");
    mc.writeToScreen("\033]133;C\033\\");
    mc.steadyClock.advance(2s);
    mc.writeToScreen("done\r\n\033]133;D;3\033\\");

    REQUIRE(mc.finishedCommandBlocks.size() == 1);
    auto const& summary = mc.finishedCommandBlocks.front();
    CHECK(summary.commandLine.raw() == "make");
    CHECK(summary.exitCode == 3);
    CHECK(summary.duration == 2s);

    // A second ;D with nothing running (tcsh) announces nothing more.
    mc.writeToScreen("\033]133;D;0\033\\");
    CHECK(mc.finishedCommandBlocks.size() == 1);
}

TEST_CASE("Terminal.blocks.everyChangeToABlockIsSaidWhateverItsCause", "[terminal][semanticblocks]")
{
    // What a view showing a record re-reads it on. The ends commandBlockFinished never announces are here
    // too: one a newer prompt implies, and an OSC 3008 end= that tells after ;D how the command died.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    auto const currentId = [&] {
        return mc.terminal.commandBlocks().currentId();
    };

    // Each change says which block changed, and which is then the newest block to have run a command.
    auto const told = [](CommandBlockId id, CommandBlockId lastCommand) {
        return CommandBlockChange { .id = id, .lastCommand = lastCommand };
    };

    typeAtPrompt(mc, "ssh host");
    auto const ssh = currentId();
    CHECK(mc.changedCommandBlocks.empty()); // a birth changes nothing anyone has read

    mc.writeToScreen("\033]133;C\033\\");
    CHECK(mc.changedCommandBlocks == std::vector { told(ssh, ssh) });

    mc.writeToScreen("\033]133;A\033\\"); // the remote shell's prompt
    CHECK(mc.changedCommandBlocks == std::vector { told(ssh, ssh), told(ssh, ssh) });
    CHECK(mc.finishedCommandBlocks.empty());

    mc.changedCommandBlocks.clear();
    typeAtPrompt(mc, "serve");
    auto const serve = currentId();
    mc.writeToScreen("\033]3008;start=cmd-1;type=command\033\\\033]133;C\033\\\033]133;D;1\033\\");
    CHECK(mc.changedCommandBlocks == std::vector { told(serve, serve), told(serve, serve) });
    CHECK(mc.finishedCommandBlocks.size() == 1);

    // How the command died arrives after the next prompt: it is still the newest command.
    mc.writeToScreen("\033]133;A\033\\\033]3008;end=cmd-1;exit=interrupt;signal=SIGTERM\033\\");
    CHECK(mc.changedCommandBlocks
          == std::vector { told(serve, serve), told(serve, serve), told(serve, serve) });
    CHECK(mc.finishedCommandBlocks.size() == 1);
}

TEST_CASE("Terminal.blocks.theExitCodeIsTheFirstParameterOfACommandEnd", "[terminal][semanticblocks]")
{
    // WezTerm follows the exit code with key=value parameters. Read as one number, the whole tail is none,
    // and a failed command was recorded -- and announced -- as a success.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    typeAtPrompt(mc, "false");
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen("\033]133;D;1;aid=42\033\\");

    REQUIRE(mc.finishedCommandBlocks.size() == 1);
    CHECK(mc.finishedCommandBlocks.front().exitCode == 1);
    auto const* record = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    CHECK(record->exitCode == 1);
    CHECK(outcomeOf(*record) == CommandBlockOutcome::Failure);
}

TEST_CASE("Terminal.blocks.aNestedPromptClosesTheRunningBlockWithoutAnnouncingIt",
          "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    typeAtPrompt(mc, "ssh host");
    mc.writeToScreen("\033]133;C\033\\");
    mc.writeToScreen("\033]133;A\033\\remote$ "); // the remote shell's own prompt

    CHECK(mc.finishedCommandBlocks.empty());
    auto const* outer = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(outer != nullptr);
    CHECK(outer->end == CommandBlockEnd::Implicit);
    CHECK(outer->commandLine.raw() == "ssh host");
}

TEST_CASE("Terminal.blocks.theWorkingDirectoryIsSnapshottedAtTheCommandStart", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };

    SECTION("from OSC 7")
    {
        mc.writeToScreen("\033]7;file:///home/user/src\033\\");
        typeAtPrompt(mc, "make");
        mc.writeToScreen("\033]133;C\033\\");
        // A later OSC 7 (the next precmd) must not rewrite where this command ran.
        mc.writeToScreen("\033]7;file:///tmp\033\\");

        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        CHECK(record->workingDirectory.path == "/home/user/src");
        CHECK(record->workingDirectory.locality == ContextLocality::Local);
    }

    SECTION("from OSC 7 naming another host")
    {
        mc.writeToScreen("\033]7;file://elsewhere/home/user\033\\");
        typeAtPrompt(mc, "make");
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->workingDirectory.path == "/home/user");
        CHECK(mc.terminal.commandBlocks().current()->workingDirectory.locality == ContextLocality::Foreign);
    }

    SECTION("from an OSC 3008 context this machine's identity vouches for")
    {
        mc.identity.machineId = "3deb5353d3ba43d08201c136a47ead7b";
        mc.writeToScreen(
            "\033]3008;start=shell-1;type=shell;machineid=3deb5353d3ba43d08201c136a47ead7b;cwd=/srv\033\\");
        typeAtPrompt(mc, "make");
        mc.writeToScreen("\033]133;C\033\\");
        REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
        CHECK(mc.terminal.commandBlocks().current()->workingDirectory.path == "/srv");
        CHECK(mc.terminal.commandBlocks().current()->workingDirectory.locality == ContextLocality::Local);
    }
}

TEST_CASE("Terminal.blocks.anOsc7PathIsDecodedOnceAndJudgedByItsHost", "[terminal][semanticblocks]")
{
    // How the shell integrations send a directory: its path percent-encoded, after this machine's name.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    mc.identity.hostname = "fedora";
    auto const startedIn = [&](std::string_view osc7) {
        mc.writeToScreen(std::format("\033]7;{}\033\\", osc7));
        typeAtPrompt(mc, "make");
        mc.writeToScreen("\033]133;C\033\\");
        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        return record->workingDirectory;
    };

    SECTION("this machine's: decoded once -- an escaped '%' stays a '%'")
    {
        auto const directory = startedIn("file://fedora/tmp/50%2541%20%1B%07");
        CHECK(directory.path == "/tmp/50%41 \x1b\x07");
        CHECK(directory.locality == ContextLocality::Local);
    }

    SECTION("another machine's: decoded alike, and foreign")
    {
        auto const directory = startedIn("file://elsewhere/tmp/a%20b");
        CHECK(directory.path == "/tmp/a b");
        CHECK(directory.locality == ContextLocality::Foreign);
    }

    SECTION("one whose path decodes to hold a NUL names no directory, and is not taken")
    {
        mc.writeToScreen("\033]7;file://fedora/srv\033\\");
        auto const directory = startedIn("file://fedora/srv%00/etc");
        CHECK(directory.path == "/srv");
        CHECK(mc.terminal.currentWorkingDirectory() == "file://fedora/srv");
    }
}

TEST_CASE("Terminal.blocks.theAlternateScreenFeedsBlocksButStampsNoRow", "[terminal][semanticblocks]")
{
    // The store hears the shell on the alternate screen too, but rows are the primary screen's alone: a head
    // id taken on the alternate grid would name a row of a different ring. So a block opened there is headed
    // where the shell's output continues once the alternate screen exits -- the primary cursor's row.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    mc.writeToScreen("before\r\n");
    auto const& primary = mc.terminal.primaryScreen().grid();
    auto const primaryCursorRow = mc.terminal.primaryScreen().cursor().position.line;
    mc.writeToScreen("\033[?1049h");
    mc.writeToScreen("\033]133;A\033\\$ \033]133;C\033\\x\r\n\033]133;D;0\033\\");

    REQUIRE(mc.terminal.commandBlocks().size() == 1);
    CHECK(mc.finishedCommandBlocks.size() == 1);
    auto const* record = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(record != nullptr);
    CHECK(record->headStableId == primary.stableLineIdOf(primaryCursorRow));
    CHECK(record->headIdGeneration == primary.stableIdGeneration());
    CHECK(primary.lineAt(primaryCursorRow).blockId() == record->id);
    for (auto const row: std::views::iota(0, 6))
        CHECK(mc.terminal.alternateScreen().grid().lineAt(LineOffset(row)).blockId() == CommandBlockId {});
}

TEST_CASE("Terminal.blocks.aShellResumingOnTheAlternateScreenStillEndsItsBlock", "[terminal][semanticblocks]")
{
    // A full-screen program that dies without leaving the alternate screen (vim on SIGSEGV: no rmcup) hands
    // the shell back a terminal still showing it, so the shell's command end and its next prompt arrive
    // there. The block must still finish -- with the crash's exit code, announced once -- not run forever.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    typeAtPrompt(mc, "vim");
    mc.writeToScreen("\033]133;C\033\\");
    auto const vim = mc.terminal.commandBlocks().currentId();
    mc.writeToScreen("\033[?1049h~\r\n~\r\n"); // vim's screen, then the crash
    mc.writeToScreen("\033]133;D;139\033\\\033]133;A\033\\$ ");

    REQUIRE(mc.terminal.isAlternateScreen());
    REQUIRE(mc.finishedCommandBlocks.size() == 1);
    CHECK(mc.finishedCommandBlocks.front().id == vim);
    CHECK(mc.finishedCommandBlocks.front().exitCode == 139);
    auto const* record = mc.terminal.commandBlocks().find(vim);
    REQUIRE(record != nullptr);
    CHECK(record->state == CommandBlockState::Finished);
    CHECK(record->end == CommandBlockEnd::Reported);
    CHECK(record->exitCode == 139);

    // ...and the prompt opened the next block.
    auto const* next = mc.terminal.commandBlocks().current();
    REQUIRE(next != nullptr);
    CHECK(next->id != vim);
    CHECK(next->state == CommandBlockState::Prompting);
}

TEST_CASE("Terminal.blocks.aDecPageOrAStatusLineFeedsNoBlock", "[terminal][semanticblocks]")
{
    // Only the main display -- the primary or the alternate screen -- is where the shell's command cycle
    // runs.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };

    SECTION("a DEC page other than the first")
    {
        mc.writeToScreen("\033[2 P"); // PPA 2
    }
    SECTION("the host-writable status line")
    {
        mc.writeToScreen("\033[1$}"); // DECSASD: to the status line
    }

    mc.writeToScreen("\033]133;A\033\\$ \033]133;C\033\\x\r\n\033]133;D;0\033\\");

    CHECK(mc.terminal.commandBlocks().size() == 0);
    CHECK(mc.finishedCommandBlocks.empty());
}

TEST_CASE("Terminal.blocks.aHardResetForgetsEveryBlock", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    runCommand(mc, "ls", { "x" });
    REQUIRE(mc.terminal.commandBlocks().size() == 1);

    mc.writeToScreen("\033c");

    CHECK(mc.terminal.commandBlocks().size() == 0);
    CHECK(mc.terminal.commandBlocks().currentId() == CommandBlockId {});
    CHECK(blockOfRow(mc, 0) == CommandBlockId {});
}

TEST_CASE("Terminal.blocks.aHardResetMidCommandDropsTheRunningBlock", "[terminal][semanticblocks]")
{
    // A command that sends RIS while it runs -- `reset`, or a program dying mid-draw -- takes the running
    // block with it, as RIS takes every block. The shell's ;D for it, arriving after the reset, finds no
    // block to close: it is ignored, and announces no finish. The next prompt starts afresh.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    startCommand(mc, "reset");
    mc.writeToScreen("out\r\n");
    auto const* const running = mc.terminal.commandBlocks().current();
    REQUIRE(running != nullptr);
    REQUIRE(running->state == CommandBlockState::Running);
    auto const resetId = running->id;

    mc.writeToScreen("\033c");
    CHECK(mc.terminal.commandBlocks().size() == 0);
    CHECK(mc.terminal.commandBlocks().current() == nullptr);

    mc.writeToScreen(commandEnd(0));
    CHECK(mc.terminal.commandBlocks().size() == 0);
    CHECK(mc.terminal.commandBlocks().find(resetId) == nullptr);
    CHECK(mc.finishedCommandBlocks.empty());

    runCommand(mc, "ls", { "x" });
    REQUIRE(mc.finishedCommandBlocks.size() == 1);
    CHECK(mc.finishedCommandBlocks.front().id != resetId);
    CHECK(mc.finishedCommandBlocks.front().commandLine.raw() == "ls");
}

TEST_CASE("Terminal.blocks.anIdFromBeforeAHardResetNamesNoLaterBlock", "[terminal][semanticblocks]")
{
    // A reset forgets the records but does not start the ids over, so an id handed out before it -- to a
    // row, to a daemon mirror's copy of that row, to a mode 2034 reader -- resolves to nothing afterwards
    // rather than to whatever block came to carry the same number.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) } };
    runCommand(mc, "ls", { "x" });
    auto const before = blockOfRow(mc, 0);
    REQUIRE(before != CommandBlockId {});

    mc.writeToScreen("\033c");
    runCommand(mc, "pwd", { "y" });

    auto const* const after = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(after != nullptr);
    CHECK(after->id > before);
    CHECK(mc.terminal.commandBlocks().find(before) == nullptr);
}

TEST_CASE("Terminal.blocks.aFloodOfPromptsStaysWithinTheCap", "[terminal][semanticblocks]")
{
    // A `cat` of a log that contains OSC 133 replays every cycle in it: the store stays within its cap, and
    // every reported finish is still announced.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) }, LineCount(100) };
    auto const cycle = std::string { "\033]133;A\033\\$ \033]133;C\033\\x\r\n\033]133;D;0\033\\" };
    auto flood = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 2'500))
        flood += cycle;
    mc.writeToScreen(flood);

    CHECK(mc.terminal.commandBlocks().size() == mc.terminal.commandBlocks().limits().maxRecords);
    CHECK(mc.finishedCommandBlocks.size() == 2'500);
}

TEST_CASE("Terminal.blocks.aFloodOfCyclesOnOneRowCostsAReadOneWalk", "[terminal][semanticblocks]")
{
    // `\e]133;A\e\\\e]133;C\e\\\e]133;D;0\e\\` over and over with no newline: each prompt takes the one row
    // of the block before, and only a walk of the whole history tells that the block has no row left. The
    // cycles leave that walk to the next read, which makes it once for all of them; a read after it has
    // nothing left to do.
    constexpr auto PageLines = 6;
    constexpr auto HistoryLines = 500;
    auto mc = MockTerm { PageSize { LineCount(PageLines), ColumnCount(40) }, LineCount(HistoryLines) };
    printLines(mc, "history", HistoryLines + PageLines);
    std::ignore = mc.terminal.commandBlocks();
    auto const settled = mc.terminal.commandBlockHeadWork();

    auto flood = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 300))
        flood += "\033]133;A\033\\\033]133;C\033\\\033]133;D;0\033\\";
    mc.writeToScreen(flood);
    CHECK(mc.terminal.commandBlockHeadWork() == settled); // the cycles themselves re-head nothing

    auto const& blocks = mc.terminal.commandBlocks();
    auto const read = mc.terminal.commandBlockHeadWork() - settled;
    // Each record looked at as the read looks for heads in doubt and again as it counts those gone; each row
    // walked once.
    CHECK(read <= (2 * blocks.size()) + HistoryLines + PageLines);

    std::ignore = mc.terminal.commandBlocks();
    CHECK(mc.terminal.commandBlockHeadWork() == settled + read);

    // Every block but the newest lost its only row to the prompt after it.
    auto const& grid = mc.terminal.primaryScreen().grid();
    CHECK(std::ranges::count_if(blocks.records(),
                                [&](CommandBlockRecord const& record) { return isHeadEvicted(grid, record); })
          == static_cast<std::ptrdiff_t>(blocks.size() - 1));
}

TEST_CASE("Terminal.blocks.rowsStampedBeforeTheIdsStartOverNameNoBlock", "[terminal][semanticblocks]")
{
    // When the ids run out the store starts them over at 1, so the next blocks reuse numbers the rows
    // already carry. Those rows must not then resolve to the new blocks.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) } };
    runCommand(mc, "ls", { "a" }); // rows 0 and 1; the cursor is on row 2
    auto const* finished = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(finished != nullptr);
    auto const first = finished->id;

    // A finished record a daemon host handed over, numbered just below the wrap: the next block minted starts
    // the ids over.
    auto record = *finished;
    record.id = CommandBlockId { std::numeric_limits<CommandBlockId::inner_type>::max() - 1 };
    std::ignore = mc.terminal.commandBlocks().adopt(record, AdoptMode::Snapshot);

    SECTION("minted by a prompt")
    {
        mc.writeToScreen("\033]133;A\033\\$ ");
    }
    SECTION("minted by a command start with no prompt before it")
    {
        mc.writeToScreen("\033]133;C\033\\");
    }

    auto const fresh = mc.terminal.commandBlocks().currentId();
    REQUIRE(fresh == first); // the old number, naming a new block
    CHECK(blockOfRow(mc, 0) == CommandBlockId {});
    CHECK(blockOfRow(mc, 1) == CommandBlockId {});
    CHECK(mc.terminal.commandBlockAt(LineOffset(0)) == nullptr);
    CHECK(blockOfRow(mc, 2) == fresh); // the new block's own row
}

TEST_CASE("Terminal.blocks.commandBlockAtAnswersNoBlockWhereThereIsNone", "[terminal][semanticblocks]")
{
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) } };
    mc.writeToScreen("plain output\r\n");

    CHECK(mc.terminal.commandBlockAt(LineOffset(0)) == nullptr);   // no shell integration: no block
    CHECK(mc.terminal.commandBlockAt(LineOffset(99)) == nullptr);  // below the page
    CHECK(mc.terminal.commandBlockAt(LineOffset(-99)) == nullptr); // above the scrollback
}

TEST_CASE("Terminal.blocks.aRowAnEraseBlankedBelongsToTheBlockOfItsFold", "[terminal][semanticblocks]")
{
    // An erase below row 1 takes rows 2-4 from the running block while the cursor is elsewhere. They lie in
    // its fold all the same, so they still belong to it -- for the gutter and every other consumer alike.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    startCommand(mc, "false");                    // head on row 0; the output starts on row 1
    mc.writeToScreen("a\r\nb\r\nc\r\nd\r\n");     // rows 1-4
    mc.writeToScreen("\033[2;1H\033[J\033[6;1H"); // erase below row 1, then step over rows 2-4 to row 5
    mc.writeToScreen(commandEnd(1));
    REQUIRE(blockOfRow(mc, 2) == CommandBlockId {});

    auto const* const block = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(block != nullptr);
    for (auto const row: std::views::iota(1, 6))
    {
        CAPTURE(row);
        CHECK(mc.terminal.commandBlockAt(LineOffset(row)) == block);
    }
    CHECK(mc.terminal.commandBlockAt(LineOffset(7)) == nullptr); // below the fold: still no block
}

TEST_CASE("Terminal.blocks.aRowAnEraseBlankedAboveTheCursorBelongsToTheRunningBlock",
          "[terminal][semanticblocks]")
{
    // The command erases the page above the cursor (ED 1) while its prompt is in the history. No fold says
    // whose the erased rows are -- a block folds once it finishes -- but they lie between the running block's
    // head and the cursor, so they are its rows.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(10) };
    startCommand(mc, "make"); // the head, which the output scrolls into the history
    for (auto const i: std::views::iota(1, 9))
        mc.writeToScreen(std::format("{}\r\n", i)); // the page holds 4 to 8 and the cursor's row
    mc.writeToScreen("\033[4;1H\033[1J");           // erase rows 0-2, and row 3 up to the cursor
    REQUIRE(blockOfRow(mc, 1) == CommandBlockId {});

    auto const* const block = mc.terminal.commandBlocks().current();
    REQUIRE(block != nullptr);
    REQUIRE(mc.terminal.commandBlockAt(LineOffset(-4)) == block); // the head
    for (auto const row: std::views::iota(0, 3))
    {
        CAPTURE(row);
        CHECK(mc.terminal.commandBlockAt(LineOffset(row)) == block);
    }
}

TEST_CASE("Terminal.blocks.aRunningBlockWhoseHeadLeftTheHistoryOwnsTheRowsErasedAboveTheCursor",
          "[terminal][semanticblocks]")
{
    // As above, but the prompt has left a four-line history: the head lies above every row the grid holds.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(4) };
    startCommand(mc, "seq");
    for (auto const i: std::views::iota(1, 8))
        mc.writeToScreen(std::format("{}\r\n", i));
    mc.writeToScreen("\033[1J"); // the cursor is on the page's last row: erase the three above it
    REQUIRE(blockOfRow(mc, 0) == CommandBlockId {});

    auto const* const block = mc.terminal.commandBlocks().current();
    REQUIRE(block != nullptr);
    REQUIRE_FALSE(headOffsetOf(mc.terminal.primaryScreen().grid(), *block).has_value());
    for (auto const row: std::views::iota(0, 3))
    {
        CAPTURE(row);
        CHECK(mc.terminal.commandBlockAt(LineOffset(row)) == block);
    }
}

TEST_CASE("Terminal.blocks.aFinishedBlocksErasedRowOutsideItsFoldBelongsToNoBlock",
          "[terminal][semanticblocks]")
{
    // A finished block stamps the rows the cursor reaches after its end mark, until the next prompt. Two of
    // them are erased while the cursor is above them, and the cursor then passes them: they lie below the
    // block's fold, and the block is no longer the one running, so nothing says whose they are.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.writeToScreen(commandRun("ls", { "a", "b" }) + "x\r\ny\r\nz\r\n");
    mc.writeToScreen("\033[4;2H\033[J\033[7;1Hw"sv); // erase below the end mark's row, then print on row 6
    auto const* const block = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(block != nullptr);
    REQUIRE(block->state == CommandBlockState::Finished);
    REQUIRE(blockOfRow(mc, 4) == CommandBlockId {});
    REQUIRE(blockOfRow(mc, 6) == block->id);

    CHECK(mc.terminal.commandBlockAt(LineOffset(4)) == nullptr);
    CHECK(mc.terminal.commandBlockAt(LineOffset(5)) == nullptr);
}

TEST_CASE("Terminal.blocks.aBlockActionNamesTheBlockOfTheFoldAnErasedRowLiesIn", "[semanticblocks]")
{
    // As above, but the next prompt starts on the row the command ended on, so the block's last row that
    // carries its id is row 1: rows 2-4 below it are the block's only through its fold. A block action
    // anchored on any of them names the block commandBlockAt() names, and the block's rows run through them.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    startCommand(mc, "false");                    // head on row 0; the output starts on row 1
    mc.writeToScreen("a\r\nb\r\nc\r\nd\r\n");     // rows 1-4
    mc.writeToScreen("\033[2;1H\033[J\033[6;1H"); // erase below row 1, then step over rows 2-4 to row 5
    mc.writeToScreen(commandEnd(1) + std::string { PromptStart });
    REQUIRE(blockOfRow(mc, 2) == CommandBlockId {});
    REQUIRE(blockOfRow(mc, 5) != blockOfRow(mc, 1));

    auto const* const block = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(block != nullptr);
    for (auto const row: std::views::iota(0, 5))
    {
        CAPTURE(row);
        REQUIRE(mc.terminal.commandBlockAt(LineOffset(row)) == block);
        CHECK(mc.terminal.commandBlockRowsAt(LineOffset(row), CommandBlockPart::Output)
              == CommandBlockRows { .first = LineOffset(1), .last = LineOffset(4) });
        CHECK(mc.terminal.commandBlockRowsAt(LineOffset(row), CommandBlockPart::PromptAndOutput)
              == CommandBlockRows { .first = LineOffset(0), .last = LineOffset(4) });
    }
}

TEST_CASE("Terminal.blocks.aBlockActionNamesTheRunningBlockForRowsItsCursorPassed", "[semanticblocks]")
{
    // The command erases from row 1 down while the cursor is there. Rows 2-4 then carry no block and lie
    // below the cursor, so they are not yet the running block's -- for commandBlockAt() and an action alike.
    // Once the cursor steps past them they lie between its head and the cursor, and are its rows. A next
    // prompt with no end mark before it leaves the block finished without a fold, and them no block's again.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    startCommand(mc, "make");                 // head on row 0; the output starts on row 1
    mc.writeToScreen("a\r\nb\r\nc\r\nd\r\n"); // rows 1-4
    mc.writeToScreen("\033[2;1H\033[J");      // erase from row 1 down, the cursor staying on row 1
    REQUIRE(blockOfRow(mc, 3) == CommandBlockId {});
    auto const* const running = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(running != nullptr);
    auto const id = running->id;

    CHECK(mc.terminal.commandBlockAt(LineOffset(3)) == nullptr);
    CHECK(mc.terminal.commandBlockRowsAt(LineOffset(3), CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));

    mc.writeToScreen("\033[6;1He"); // step over rows 2-4, print on row 5
    REQUIRE(blockOfRow(mc, 3) == CommandBlockId {});
    CHECK(mc.terminal.commandBlockAt(LineOffset(3)) == mc.terminal.commandBlocks().find(id));
    CHECK(mc.terminal.commandBlockRowsAt(LineOffset(3), CommandBlockPart::Output)
          == CommandBlockRows { .first = LineOffset(1), .last = LineOffset(5) });

    mc.writeToScreen("\r\n" + std::string { PromptStart }); // the next prompt, on row 6
    REQUIRE(mc.terminal.commandBlocks().find(id)->state == CommandBlockState::Finished);
    CHECK(mc.terminal.commandBlockAt(LineOffset(3)) == nullptr);
    CHECK(mc.terminal.commandBlockRowsAt(LineOffset(3), CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(mc.terminal.commandBlockRowsAt(LineOffset(1), CommandBlockPart::Output)
          == CommandBlockRows { .first = LineOffset(1), .last = LineOffset(5) });
}

TEST_CASE("Terminal.blocks.aBlockActionsOutputEndsOnTheRowTheCommandEndedOn", "[semanticblocks]")
{
    // A precmd hook writes a line after the ;D, before the next prompt's ;A. The finished block stamps that
    // row as it stamps every row the cursor reaches until a prompt starts, but the command did not print it.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) } };
    mc.writeToScreen(commandRun("ls", { "a" }) + "\r\nprecmd\r\n" + std::string { PromptStart });
    auto const* const block = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(block != nullptr);
    REQUIRE(mc.terminal.commandBlockAt(LineOffset(3)) == block); // the precmd line: the block's row ...

    // ... but not its output: that ends at the ;D, which nothing precedes on its row 2, so the output ends
    // with the row above it.
    CHECK(mc.terminal.commandBlockRowsAt(LineOffset(1), CommandBlockPart::Output)
          == CommandBlockRows { .first = LineOffset(1), .last = LineOffset(1) });
    CHECK(mc.terminal.commandBlockRowsAt(LineOffset(3), CommandBlockPart::PromptAndOutput)
          == CommandBlockRows { .first = LineOffset(0), .last = LineOffset(1) });
}

// }}}

// {{{ heads survive a reflow

namespace
{

TEST_CASE("Terminal.blocks.theCommandLineIsRecoveredAfterANarrowingReflowedThePrompt",
          "[terminal][semanticblocks]")
{
    // The window narrows while the shell waits at a prompt it said it does not redraw (redraw=0), so the
    // reflow re-chops the prompt into rows of the new width; the input typed afterwards must still begin
    // where the prompt ends, or the command recovered from the grid starts with blanks or loses its first
    // characters to the prompt.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };

    auto const typeAfterNarrowing = [&](std::string_view prompt, int narrowedColumns) {
        mc.writeToScreen(std::format("\033]133;A;redraw=0\033\\{}\033]133;B\033\\", prompt));
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(narrowedColumns) });
        mc.writeToScreen("ls");
        mc.writeToScreen("\033]133;C\033\\");
        auto const* record = mc.terminal.commandBlocks().current();
        REQUIRE(record != nullptr);
        CHECK(record->commandLine.raw() == "ls");
    };

    SECTION("a prompt that now spans three rows")
    {
        typeAfterNarrowing("user@host:~$ ", 6);
    }

    SECTION("a prompt that now fills its row exactly")
    {
        typeAfterNarrowing("abcdef ", 7);
    }
}

/// One logical line of the primary grid as a reader sees it: its text, and the block and birth stamp
/// its HEAD carries -- what a reflow re-chops but must not change.
struct LogicalLineSnapshot
{
    std::string text;
    CommandBlockId block {};
    uint32_t bornAt = 0;

    bool operator==(LogicalLineSnapshot const&) const = default;
};

/// Every logical line from the top of the scrollback to the bottom of the page; blank rows the cursor
/// never reached are left out, since a reflow adds and drops those freely.
[[nodiscard]] std::vector<LogicalLineSnapshot> logicalLines(Terminal& terminal)
{
    auto& grid = terminal.primaryScreen().grid();
    auto lines = std::vector<LogicalLineSnapshot> {};
    for (auto const& logicalLine: grid.logicalLinesFrom(grid.addressableTop()))
    {
        auto const& head = logicalLine.lines.front().get();
        auto text = logicalLine.text();
        text.resize(core::trimRight(text).size());
        if (!text.empty() || head.bornAt() != 0)
            lines.push_back(LogicalLineSnapshot {
                .text = std::move(text), .block = head.blockId(), .bornAt = head.bornAt() });
    }
    return lines;
}

/// The text of every logical line that holds any, from the top of the scrollback to the bottom of the page.
[[nodiscard]] std::vector<std::string> logicalLineTexts(Terminal& terminal)
{
    return std::ranges::to<std::vector>(
        logicalLines(terminal) | std::views::transform(&LogicalLineSnapshot::text)
        | std::views::filter([](std::string const& text) { return !text.empty(); }));
}

/// A command cycle that draws nothing, its marks all on the cursor's row: it shows the terminal that this
/// shell marks where a command's output starts, which a live prompt is left unreflowed only after.
constexpr auto ShellMarksOutputStarts = "\033]133;A\033\\\033]133;C\033\\\033]133;D;0\033\\"sv;

/// A command still printing when the window is resized: a prompt at t=10s, then eight 30-column lines at
/// t=15s on a 20-column page, so every one of them wraps.
[[nodiscard]] CommandBlockId runLongCommand(MockTerm<>& mc)
{
    mc.wallClock.advance(10s);
    typeAtPrompt(mc, "make");
    mc.writeToScreen("\033]133;C\033\\");
    mc.wallClock.advance(5s);
    for (auto const i: std::views::iota(0, 8))
        mc.writeToScreen(std::format("{:-<30}\r\n", i));
    return mc.terminal.commandBlocks().currentId();
}

/// Resizes the page with a reflow that rebuilds the ring, so stable row identity dies and a new id
/// generation begins.
/// @param size The page size to resize to.
void reflowTo(MockTerm<>& mc, PageSize size)
{
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generationBefore = grid.stableIdGeneration();
    mc.terminal.resizeScreen(size);
    REQUIRE(grid.stableIdGeneration() != generationBefore);
}

/// The record's head is re-recorded in the grid's current generation, and names a row carrying @p mark.
/// @param mark What the head row carries: LineFlag::Marked for a prompt, LineFlag::OutputStart for a
///             block a ;C started.
/// @param text The head row's text, trimmed.
void checkHeadIs(MockTerm<>& mc, CommandBlockId id, LineFlag mark, std::string_view text)
{
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const* record = mc.terminal.commandBlocks().find(id);
    REQUIRE(record != nullptr);
    CHECK(record->headIdGeneration == grid.stableIdGeneration());
    auto const head = grid.lineOffsetOf(record->headStableId);
    REQUIRE(head.has_value());
    CHECK(grid.lineAt(*head).isFlagEnabled(mark));
    CHECK(grid.lineTextTrimmed(*head) == text);
}

/// The record's head is current, in the grid's current generation, and names a row of the block, whatever
/// that row carries: a block with neither mark left is headed at its topmost row.
/// @param text The head row's text, trimmed.
void checkHeadRowIs(MockTerm<>& mc, CommandBlockId id, std::string_view text)
{
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const* record = mc.terminal.commandBlocks().find(id);
    REQUIRE(record != nullptr);
    CHECK(record->headIdGeneration == grid.stableIdGeneration());
    auto const head = grid.lineOffsetOf(record->headStableId);
    REQUIRE(head.has_value());
    CHECK(grid.lineAt(*head).blockId() == id);
    CHECK(grid.lineTextTrimmed(*head) == text);
}

/// `seq 1 5; printf '\e[1J'; seq 6 8` at a fresh prompt, still running: the ED 1 blanks the page above the
/// cursor -- the prompt row and the output start's row among it -- and a row blanked away from the cursor
/// carries no block, its marks gone with it. The rows written after the erase still carry the block's id.
/// @return The block the command runs in.
[[nodiscard]] CommandBlockId runEraseAbove(MockTerm<>& mc)
{
    typeAtPrompt(mc, "seq");
    mc.writeToScreen("\033]133;C\033\\1\r\n2\r\n3\r\n4\r\n5\r\n\033[1J6\r\n7\r\n8\r\n");
    return mc.terminal.commandBlocks().currentId();
}

/// Runs `clear` at a fresh prompt on a 6-line page: the cursor homes onto the page's top row, which adopts
/// the running block if it belongs to none yet, then ED 2 scrolls the whole page -- that row included -- into
/// the history.
/// @param homedRowErase What the program sends once the cursor is home, before ED 2.
/// @return The block `clear` ran in.
[[nodiscard]] CommandBlockId runClear(MockTerm<>& mc, std::string_view homedRowErase = {})
{
    typeAtPrompt(mc, "clear");
    mc.writeToScreen(std::format("\033]133;C\033\\\033[H{}\033[2J", homedRowErase));
    return mc.terminal.commandBlocks().currentId();
}

/// Where the page's top row lies once runClear() has scrolled the 6-line page into the history.
constexpr auto HomedRowAfterClear = LineOffset(-6);

/// The rows of the primary screen's page, each without its trailing blanks.
[[nodiscard]] std::vector<std::string> pageRows(MockTerm<>& mc)
{
    auto const& grid = mc.terminal.primaryScreen().grid();
    return std::ranges::to<std::vector>(
        std::views::iota(0, unbox<int>(grid.pageSize().lines))
        | std::views::transform([&grid](int row) { return grid.lineTextTrimmed(LineOffset(row)); }));
}

/// A zsh prompt and the command line typed at it, which on a 20-column page wrap onto a second row.
constexpr auto ZshPrompt = "user@host:~/src$ "sv;
constexpr auto ZshInput = "echo hello"sv;

/// Two lines of output from a shell that marks output starts, then the prompt and its input, the prompt's
/// start marked by @p promptStart.
void drawPromptBelowOutput(MockTerm<>& mc, std::string_view promptStart)
{
    mc.writeToScreen(std::format("{}out1\r\nout2\r\n{}{}\033]133;B\033\\{}",
                                 ShellMarksOutputStarts,
                                 promptStart,
                                 ZshPrompt,
                                 ZshInput));
}

/// What zsh does on SIGWINCH: it moves up to the row its prompt began on, by as many rows as it drew the
/// prompt and its input on, erases from there to the end of the page, and draws both again.
void redrawLikeZsh(MockTerm<>& mc, int rowsAboveCursor)
{
    mc.writeToScreen(
        std::format("\033[{}A\r\033[J{}\033]133;B\033\\{}", rowsAboveCursor, ZshPrompt, ZshInput));
}

/// Presses Enter at the prompt: the command line the output start then records.
[[nodiscard]] std::string commandLineAtOutputStart(MockTerm<>& mc)
{
    mc.writeToScreen("\r\n\033]133;C\033\\");
    auto const* record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    return record->commandLine.raw();
}

} // namespace

TEST_CASE("Terminal.blocks.aResizeLeavesTheLivePromptForTheShellToRedraw", "[terminal][semanticblocks]")
{
    // The shell redraws its prompt at the new width, moving up as many rows as it drew it on. Reflowed under
    // it as well, the prompt spans other rows: narrower, the redraw starts inside it and leaves its first row
    // behind; wider, it starts above it and erases the output there. So the prompt is left as it was drawn,
    // cut at the new width or padded to it, as kitty leaves it -- and once redrawn, it is recovered from and
    // heads its block as ever.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    drawPromptBelowOutput(mc, "\033]133;A\033\\");
    auto const id = mc.terminal.commandBlocks().currentId();
    REQUIRE(pageRows(mc)
            == std::vector<std::string> { "out1", "out2", "user@host:~/src$ ech", "o hello", "", "" });

    SECTION("narrower")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
        CHECK(pageRows(mc) == std::vector<std::string> { "out1", "out2", "user@host:~/", "o hello", "", "" });

        redrawLikeZsh(mc, 1);
        CHECK(pageRows(mc)
              == std::vector<std::string> { "out1", "out2", "user@host:~/", "src$ echo he", "llo", "" });
        CHECK(commandLineAtOutputStart(mc) == ZshInput);
        checkHeadIs(mc, id, LineFlag::Marked, "user@host:~/");
    }

    SECTION("wider")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(30) });
        CHECK(pageRows(mc)
              == std::vector<std::string> { "out1", "out2", "user@host:~/src$ ech", "o hello", "", "" });

        redrawLikeZsh(mc, 1);
        CHECK(pageRows(mc)
              == std::vector<std::string> { "out1", "out2", "user@host:~/src$ echo hello", "", "", "" });
        CHECK(commandLineAtOutputStart(mc) == ZshInput);
        checkHeadIs(mc, id, LineFlag::Marked, "user@host:~/src$ echo hello");
    }
}

TEST_CASE("Terminal.blocks.aPromptTheShellDoesNotRedrawIsReflowed", "[terminal][semanticblocks]")
{
    // kitty's OSC 133;A;redraw=0: the shell leaves its prompt as drawn on a resize, so it is reflowed like
    // any other text, the cursor kept on its cell. Each prompt says it for itself.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };

    SECTION("said by this prompt")
    {
        drawPromptBelowOutput(mc, "\033]133;A;redraw=0\033\\");
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
        CHECK(pageRows(mc)
              == std::vector<std::string> { "out1", "out2", "user@host:~/", "src$ echo he", "llo", "" });
        CHECK(mc.terminal.primaryScreen().cursor().position
              == CellLocation { .line = LineOffset(4), .column = ColumnOffset(3) });
    }

    SECTION("said by an earlier prompt only")
    {
        mc.writeToScreen(
            "\033]133;A;redraw=0\033\\$ \033]133;B\033\\ls\r\n\033]133;C\033\\\033]133;D;0\033\\");
        drawPromptBelowOutput(mc, "\033]133;A\033\\");
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
        CHECK(pageRows(mc)
              == std::vector<std::string> { "$ ls", "out1", "out2", "user@host:~/", "o hello", "" });
    }
}

TEST_CASE("Terminal.blocks.aPromptThatBeganAboveThePageIsReflowed", "[terminal][semanticblocks]")
{
    // kitty looks for the live prompt on the page alone: one whose first row has gone into the history is
    // reflowed, the cursor kept on its cell.
    auto mc = MockTerm { PageSize { LineCount(2), ColumnCount(10) }, LineCount(10) };
    mc.writeToScreen("\033]133;A\033\\abcdefghijklmnopqrstuvwx"); // three rows, the first in the history

    mc.terminal.resizeScreen(PageSize { LineCount(2), ColumnCount(5) });

    CHECK(pageRows(mc) == std::vector<std::string> { "pqrst", "uvwx" });
    CHECK(mc.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(1), .column = ColumnOffset(4) });
}

TEST_CASE("Terminal.blocks.aRunningCommandsOutputIsStillReflowed", "[terminal][semanticblocks]")
{
    // Once its output starts, the cursor stands in the command's output rather than in a prompt: a resize
    // reflows as ever, the prompt above it included.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    drawPromptBelowOutput(mc, "\033]133;A\033\\");
    mc.writeToScreen("\r\n\033]133;C\033\\" + std::string(25, 'x'));

    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(30) });

    CHECK(pageRows(mc)
          == std::vector<std::string> {
              "out1", "out2", "user@host:~/src$ echo hello", std::string(25, 'x'), "", "" });
}

TEST_CASE("Terminal.blocks.untilTheShellMarksAnOutputStartAResizeReflowsEveryRow",
          "[terminal][semanticblocks]")
{
    // A shell that sends only ;A -- a foot-style PS1 -- never says where a command's output begins, so the
    // rows a running command prints read as more of the prompt above them. Until the shell has marked an
    // output start since the last hard reset, a resize reflows those rows as it reflows any text: `ping`,
    // narrowed while it runs, has its lines whole again once widened.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    auto const pingNarrowedAndWidened = [&mc] {
        mc.writeToScreen("\033]133;A\033\\$ ping host\r\n");
        for (auto const i: std::views::iota(0, 3))
            mc.writeToScreen(std::format("64 bytes: seq={}\r\n", i));
        auto const before = logicalLineTexts(mc.terminal);
        REQUIRE(before
                == std::vector<std::string> {
                    "$ ping host", "64 bytes: seq=0", "64 bytes: seq=1", "64 bytes: seq=2" });

        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(10) });
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(20) });

        CHECK(logicalLineTexts(mc.terminal) == before);
    };

    SECTION("a shell that has sent only ;A")
    {
        pingNarrowedAndWidened();
    }

    SECTION("a shell whose output start a hard reset forgot")
    {
        runCommand(mc, "ls", { "x" });
        mc.writeToScreen("\033c");
        pingNarrowedAndWidened();
    }
}

TEST_CASE("Terminal.blocks.anOsc3008CommandStartShowsTheShellMarksOutputStarts", "[terminal][semanticblocks]")
{
    // systemd's OSC 3008 marks an output start as ;C does, with its command context: from then on the live
    // prompt it marks is left for the shell to redraw.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    mc.writeToScreen("\033]3008;start=sh;type=shell\033\\$ ls\r\n");
    mc.writeToScreen("\033]3008;start=cmd-1;type=command\033\\x\r\n\033]3008;end=cmd-1;exit=success\033\\");
    mc.writeToScreen(std::format("\033]3008;start=sh;type=shell\033\\{}{}", ZshPrompt, ZshInput));
    REQUIRE(pageRows(mc)
            == std::vector<std::string> { "$ ls", "x", "user@host:~/src$ ech", "o hello", "", "" });

    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });

    CHECK(pageRows(mc) == std::vector<std::string> { "$ ls", "x", "user@host:~/", "o hello", "", "" });
}

TEST_CASE("Terminal.blocks.aDeccolmSwitchReflowsLikeAResize", "[terminal][semanticblocks]")
{
    // DECCOLM changes the page's width as a window resize does, and DECNCSM keeps the page through it: the
    // output of a shell that marks no output start is reflowed, and the live prompt of one that does is left
    // for the shell to redraw.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(80) }, LineCount(50) };
    mc.writeToScreen("\033[?40h\033[?95h\033[?3h"); // allow DECCOLM, keep the page across it, 132 columns
    REQUIRE(mc.terminal.pageSize().columns == ColumnCount(132));
    auto const toEightyColumnsAndBack = [&mc] {
        mc.writeToScreen("\033[?3l");
        REQUIRE(mc.terminal.pageSize().columns == ColumnCount(80));
        mc.writeToScreen("\033[?3h");
        REQUIRE(mc.terminal.pageSize().columns == ColumnCount(132));
    };

    SECTION("a shell that has sent only ;A: its output is reflowed")
    {
        mc.writeToScreen("\033]133;A\033\\$ cat log\r\n" + std::string(100, 'x') + "\r\n");
        auto const before = logicalLineTexts(mc.terminal);
        REQUIRE(before == std::vector<std::string> { "$ cat log", std::string(100, 'x') });

        toEightyColumnsAndBack();

        CHECK(logicalLineTexts(mc.terminal) == before);
    }

    SECTION("a shell that marks output starts: its live prompt is left as drawn")
    {
        mc.writeToScreen(std::string { ShellMarksOutputStarts } + "\033]133;A\033\\" + std::string(90, '>')
                         + "\033]133;B\033\\");

        mc.writeToScreen("\033[?3l");

        CHECK(pageRows(mc).front() == std::string(80, '>'));
        CHECK(pageRows(mc)[1].empty());
    }
}

TEST_CASE("Terminal.blocks.theInputBeginsWhereItWasDrawnUntilThePromptIsRedrawn",
          "[terminal][semanticblocks]")
{
    // Until the shell redraws it -- or for good, where it never does -- the prompt's rows are as drawn, and
    // the input on them still begins where the prompt's end was marked: on the row and at the column it was
    // drawn.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    mc.writeToScreen(std::string { ShellMarksOutputStarts }
                     + "\033]133;A\033\\user@host:~/work/src$ \033]133;B\033\\ls"); // ;B on the second row
    auto const promptAfterResizingTo = [&](int columns) {
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(columns) });
        auto const span = mc.terminal.livePromptSpan();
        REQUIRE(span.has_value());
        CHECK(span->firstLine == LineOffset(0));
        CHECK(span->lastLine == LineOffset(1));
        return span->inputBegin;
    };

    SECTION("narrower")
    {
        CHECK(promptAfterResizingTo(15) == ColumnOffset(15 + 2));
        CHECK(commandLineAtOutputStart(mc) == "ls");
    }

    SECTION("wider")
    {
        CHECK(promptAfterResizingTo(30) == ColumnOffset(30 + 2));
        CHECK(commandLineAtOutputStart(mc) == "ls");
    }
}

TEST_CASE("Terminal.blocks.aRightPromptTheShellMarkedIsKnownAfterAResizeAtThePrompt",
          "[terminal][semanticblocks]")
{
    // A resize names every line anew, so what the shell said about the prompt's lines (OSC 133;P;k=r) would
    // be lost with the old names; one that leaves the prompt as drawn hands it on. Widened, the right prompt
    // no longer reaches the margin, and its looks alone would not tell it from the command.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    mc.writeToScreen(std::string { ShellMarksOutputStarts } + "\033]133;A\033\\$ \033]133;B\033\\");
    mc.writeToScreen("\0337\033[10G\033]133;P;k=r\033\\~/src 10:42\0338"); // DECSC, CHA, DECRC
    mc.writeToScreen("ls -la");

    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(30) });

    CHECK(commandLineAtOutputStart(mc) == "ls -la");
}

TEST_CASE("Terminal.blocks.thePromptsMarksFollowAScrollInsideMarginsAtThePrompt",
          "[terminal][semanticblocks]")
{
    // What the shell said about the prompt's lines -- where a right prompt begins (;P;k=r), that the input
    // ends with its line (;I) -- is kept by the stable id of the line's head. A scroll region below a status
    // line scrolls up a row (SU) while the shell is at the prompt, moving the prompt up while each id keeps
    // naming its place: the marks must move with the prompt, or the command line read off the screen at ;C
    // takes in the right prompt, or the line below.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) }, LineCount(10) };
    auto const& grid = mc.terminal.primaryScreen().grid();
    mc.writeToScreen("status\r\nlog\r\n");

    SECTION("a right prompt the shell marked")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
        mc.writeToScreen("\0337\033[10G\033]133;P;k=r\033\\~/src 10:42\0338"); // DECSC, CHA, DECRC
        mc.writeToScreen("ls -la");
        mc.writeToScreen(
            "\033[2;8r\033[S\033[2;9H"); // the region below the status line scrolls; back to the input
        REQUIRE(grid.lineTextTrimmed(LineOffset(1)) == "$ ls -la ~/src 10:42");

        CHECK(commandLineAtOutputStart(mc) == "ls -la");
    }

    SECTION("an input the shell ended with its line")
    {
        mc.writeToScreen("\033]133;A\033\\$ \033]133;I\033\\ls\r\nfile");
        mc.writeToScreen("\033[2;8r\033[S\033[3;5H");
        REQUIRE(grid.lineTextTrimmed(LineOffset(1)) == "$ ls");
        REQUIRE(grid.lineTextTrimmed(LineOffset(2)) == "file");

        CHECK(commandLineAtOutputStart(mc) == "ls");
    }
}

TEST_CASE("Terminal.blocks.aCursorOnThePromptKeepsItsRowAsARowThatDoesNotReflowDoes",
          "[terminal][semanticblocks]")
{
    // Narrower, the cursor is held to the last column; a wrap it had pending stays pending there, so the next
    // character still goes to the next row.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    mc.writeToScreen(std::string { ShellMarksOutputStarts } + "\033]133;A\033\\" + std::string(18, '>')
                     + " \033]133;B\033\\x"); // fills the row

    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
    CHECK(mc.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(0), .column = ColumnOffset(11) });

    mc.writeToScreen("y");
    CHECK(pageRows(mc) == std::vector<std::string> { std::string(12, '>'), "y", "", "", "", "" });
}

TEST_CASE("Terminal.blocks.whereThePromptEndedMovesWithItsTextWhenAReflowPadsARow",
          "[terminal][semanticblocks]")
{
    // A wide character that does not fit the rest of its row moves on to the next whole, leaving padding at
    // the row's end, which a reflow adds where it cuts before one and drops where it joins the rows again.
    // The cells after the padding move one column on or back, and the column ;B marked must move with them.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(10) };
    auto const promptAndInput =
        std::string_view { "\033]133;A;redraw=0\033\\ABCD\xE4\xB8\xAD> \033]133;B\033\\ls" };

    SECTION("narrowed: padding added")
    {
        mc.writeToScreen(promptAndInput);
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(5) });
        CHECK(commandLineAtOutputStart(mc) == "ls");
    }

    SECTION("widened: padding dropped")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(5) });
        mc.writeToScreen(promptAndInput); // drawn at 5 columns, the wide character wrapped early
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(10) });
        CHECK(commandLineAtOutputStart(mc) == "ls");
    }

    SECTION("narrowed: padding dropped, the column past the text")
    {
        // An earlier prompt whose command line was left empty: ;B's column is past the text, where a cut
        // carries nothing on to a row below.
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(5) });
        mc.writeToScreen("\033]133;A;redraw=0\033\\ABCD\xE4\xB8\xAD> \033]133;B\033\\\r\n\033]133;A\033\\$ ");
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(4) });

        // "ABCD" "中> " "$ ": the column named the cell after "中> ", now the first of a row the line lacks.
        CHECK(pageRows(mc) == std::vector<std::string> { "ABCD", "\xE4\xB8\xAD>", "$", "", "", "" });
        CHECK(mc.terminal.primaryScreen().grid().lineAt(LineOffset(0)).promptEndOffset()
              == ColumnOffset(4 + 4));
    }
}

TEST_CASE("Terminal.blocks.whereACommandEndedMovesWithItsTextWhenAReflowPadsARow",
          "[terminal][semanticblocks]")
{
    // The same for ;D's column, on the line the command's output ended and the next prompt began on: it names
    // the cell after the output's last character, the "$" -- row 1 of the line, column 3, at 5 columns.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(10) }, LineCount(10) };
    auto const run = std::string_view { "\033]133;A\033\\$ \033]133;B\033\\cmd\r\n\033]133;C\033\\"
                                        "ABCD\xE4\xB8\xAD"
                                        "E\033]133;D;0\033\\\033]133;A;redraw=0\033\\$ " };
    auto const& grid = mc.terminal.primaryScreen().grid();

    SECTION("narrowed: padding added")
    {
        mc.writeToScreen(run);
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(5) });
        CHECK(grid.lineAt(LineOffset(1)).commandEndOffset() == ColumnOffset(5 + 3));
    }

    SECTION("widened: padding dropped")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(5) });
        mc.writeToScreen(run);
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(10) });
        CHECK(grid.lineAt(LineOffset(1)).commandEndOffset() == ColumnOffset(7));
    }
}

TEST_CASE("Terminal.blocks.aRowCutRatherThanReflowedHoldsWhereThePromptEndedToItsLastColumn",
          "[terminal][semanticblocks]")
{
    // With text reflow off (DECRST 2028), a narrowing cuts the prompt's row rather than reflowing it. ;B's
    // column, past the new width, is held to the row's last column -- where the cursor goes too -- so the
    // input typed there is still the command.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(10) };
    mc.writeToScreen("\033[?2028l\033]133;A;redraw=0\033\\user@host:~/src$ \033]133;B\033\\");

    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(10) });

    auto const span = mc.terminal.livePromptSpan();
    REQUIRE(span.has_value());
    CHECK(span->inputBegin == ColumnOffset(9));
    mc.writeToScreen("x");
    CHECK(commandLineAtOutputStart(mc) == "x");
}

TEST_CASE("Terminal.blocks.aSelectionEndingAtAnEndMarkPastItsLineStaysOnTheLine",
          "[terminal][semanticblocks]")
{
    // An end mark counted past the rows its line has -- as a narrowing that cuts rather than reflows can
    // leave one -- still ends a selection on that line's last row, not on the row below: another block's.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(10) };
    mc.writeToScreen("\033]133;A\033\\$ printf\r\n\033]133;C\033\\0123456789AB");
    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ echo x\r\n\033]133;C\033\\x\r\n");
    mc.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");
    auto const* const printfBlock = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(printfBlock != nullptr);
    mc.terminal.primaryScreen().grid().lineAt(LineOffset(1)).setCommandEndOffset(ColumnOffset(30));

    REQUIRE(
        mc.terminal.selectCommandBlock(CommandBlockTarget::Last, CommandBlockPart::Output, printfBlock->id)
            .has_value());
    CHECK(mc.terminal.selector()->to() == CellLocation { .line = LineOffset(1), .column = ColumnOffset(19) });
}

TEST_CASE("Terminal.blocks.droppedRowsTakeWhatNamedThemWithThem", "[terminal][semanticblocks]")
{
    // ED 3 and ClearToPrompt drop rows. Whatever named one of them -- the viewport, the vi cursor, a hint
    // session's matches -- must not go on naming a row that is gone.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(40) }, LineCount(20) };
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const fill = [&mc] {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 10))
            mc.writeToScreen("line\r\n");
    };

    SECTION("ED 3 with the viewport scrolled back and no scroll on output")
    {
        mc.terminal.settings().autoScrollOnUpdate = false;
        fill();
        REQUIRE(mc.terminal.viewport().scrollUp(LineCount(5)));

        mc.writeToScreen("\033[3J");

        CHECK(unbox(mc.terminal.viewport().scrollOffset())
              <= unbox(mc.terminal.viewport().scrollableLineCount()));
    }

    SECTION("ED 3 with the vi cursor in the history and no scroll on output")
    {
        mc.terminal.settings().autoScrollOnUpdate = false;
        fill();
        mc.terminal.inputHandler().setMode(ViMode::Normal);
        mc.sendCharSequence("kkkkkk");
        REQUIRE(mc.terminal.normalModeCursorPosition().line < LineOffset(0));

        mc.writeToScreen("\033[3J");

        CHECK(mc.terminal.normalModeCursorPosition().line >= grid.addressableTop());
    }

    SECTION("ClearToPrompt under a hint session over the history")
    {
        mc.writeToScreen("https://dropped.example\r\n");
        fill();
        runCommand(mc, "ls", { "a" });
        mc.writeToScreen(PromptStart);
        mc.terminal.activateHintMode(HintModeRequest { .patterns = HintModeHandler::builtinPatterns(),
                                                       .action = HintAction::Copy,
                                                       .scope = HintScope::Scrollback,
                                                       .scrollbackLimit = LineCount(1000) });
        REQUIRE_FALSE(mc.terminal.hintMatches().empty());

        REQUIRE(mc.terminal.clearToPrompt().has_value());

        for (auto const& match: mc.terminal.hintMatches())
            CHECK(match.start.line >= grid.addressableTop());
    }

    SECTION("ClearToPrompt with nothing to drop keeps the selection")
    {
        mc.writeToScreen(PromptStart);
        mc.terminal.selectAll();

        REQUIRE(mc.terminal.clearToPrompt().has_value());

        CHECK(mc.terminal.selectionAvailable());
    }
}

TEST_CASE("Terminal.blocks.aClusterContinuedAfterClearToPromptJoinsTheCellItBegan",
          "[terminal][semanticblocks]")
{
    // A combining mark at the start of the next PTY chunk joins the cell last written, which ClearToPrompt
    // moved up with its row.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) } };
    mc.writeToScreen("x\r\nx\r\n");
    mc.writeToScreen(PromptStart);
    mc.writeToScreen("e");
    REQUIRE(mc.terminal.clearToPrompt().has_value());

    mc.writeToScreen("\xcc\x81"); // U+0301 COMBINING ACUTE ACCENT

    // The mark joined the `e`, rather than taking the cell after it as a cluster of its own.
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));
    CHECK(row.toUtf8(ColumnOffset(0), ColumnOffset(3)) == "$ e\xcc\x81");
    CHECK(row.cellEmptyAt(ColumnOffset(3)) != 0);
}

TEST_CASE("Terminal.blocks.idsTimesAndHeadsSurviveAColumnResizeMidCommand", "[terminal][semanticblocks]")
{
    // The window is resized while a long command is still printing. Reflow rebuilds the ring and destroys
    // stable row identity; the running block must keep its id on every row, every row its birth time, and
    // the record its head -- re-recorded in the new id generation.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    auto const id = runLongCommand(mc);
    REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
    REQUIRE(mc.terminal.commandBlocks().current()->state == CommandBlockState::Running);

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const before = logicalLines(mc.terminal);
    REQUIRE(before.front().text == "$ make");

    SECTION("narrowing")
    {
        reflowTo(mc, PageSize { LineCount(6), ColumnCount(12) });

        CHECK(logicalLines(mc.terminal) == before);
        grid.forEachValidLine([&](LineOffset offset, Line const& line) {
            if (grid.lineTextTrimmed(offset).empty())
                return;
            CHECK(line.blockId() == id);
            CHECK((line.bornAt() == 11 || line.bornAt() == 16));
        });
        checkHeadIs(mc, id, LineFlag::Marked, "$ make");
    }

    SECTION("narrowing and widening again")
    {
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
        mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(25) });

        CHECK(logicalLines(mc.terminal) == before);
        // A widening rebuilds each logical line from its head, so every chunk carries the head's stamp.
        auto headStamp = uint32_t { 0 };
        grid.forEachValidLine([&](LineOffset, Line const& line) {
            if (!line.isFlagEnabled(LineFlag::Wrapped))
                headStamp = line.bornAt();
            else
                CHECK(line.bornAt() == headStamp);
        });
        checkHeadIs(mc, id, LineFlag::Marked, "$ make");
    }
}

TEST_CASE("Terminal.blocks.aHeadEvictedBeforeTheResizeIsRecordedAsEvicted", "[terminal][semanticblocks]")
{
    // The prompt row is gone before the resize; afterwards the record must still say so, in the NEW
    // generation's terms -- an id below the floor -- rather than adopt whatever row is oldest now.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(2) };
    typeAtPrompt(mc, "make");
    mc.writeToScreen("\033]133;C\033\\");
    for (auto const i: std::views::iota(0, 10))
        mc.writeToScreen(std::format("{:-<18}\r\n", i));
    auto const id = mc.terminal.commandBlocks().currentId();
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(mc.terminal.commandBlocks().find(id) != nullptr);
    REQUIRE_FALSE(grid.lineOffsetOf(mc.terminal.commandBlocks().find(id)->headStableId).has_value());

    reflowTo(mc, PageSize { LineCount(3), ColumnCount(10) }); // every 18-column row now wraps

    auto const* record = mc.terminal.commandBlocks().find(id);
    REQUIRE(record != nullptr);
    CHECK(record->headIdGeneration == grid.stableIdGeneration());
    CHECK(record->headStableId < grid.stableRangeFloor());
}

TEST_CASE("Terminal.blocks.aBlockStartedAfterTheResizeKeepsTheHeadItWasGiven", "[terminal][semanticblocks]")
{
    // Guards the rescan's skip: a record minted in the new generation already holds a valid head.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    mc.writeToScreen(std::string(30, 'x') + "\r\n"); // a wrapped line, so the resize rebuilds the ring
    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
    mc.writeToScreen("\033]133;A\033\\$ ");

    checkHeadIs(mc, mc.terminal.commandBlocks().currentId(), LineFlag::Marked, "$");
}

TEST_CASE("Terminal.blocks.aBlockACommandStartMintedIsReheadedAtThatRow", "[terminal][semanticblocks]")
{
    // A ;C with no ;A before it mints a block headed at the row the output starts on, which carries
    // OutputStart rather than Marked.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    mc.writeToScreen(std::string(30, 'x') + "\r\n"); // no block yet
    mc.writeToScreen("\033]133;C\033\\" + std::string(30, 'y') + "\r\n");
    auto const id = mc.terminal.commandBlocks().currentId();
    REQUIRE(id != CommandBlockId {});

    reflowTo(mc, PageSize { LineCount(6), ColumnCount(12) });

    checkHeadIs(mc, id, LineFlag::OutputStart, std::string(12, 'y'));
}

TEST_CASE("Terminal.blocks.aBlockAnEraseLeftNoMarkIsReheadedAtItsTopmostRow", "[terminal][semanticblocks]")
{
    // An ED 1 inside a command takes the prompt row and the output start's row out of the block, while the
    // rows written after the erase still carry its id, so the block is not evicted: after a reflow too, the
    // topmost of them is its head.
    auto mc = MockTerm { PageSize { LineCount(12), ColumnCount(20) }, LineCount(50) };
    auto const id = runEraseAbove(mc);
    REQUIRE(mc.terminal.primaryScreen().grid().lineAt(LineOffset(0)).blockId() == CommandBlockId {});

    reflowTo(mc, PageSize { LineCount(12), ColumnCount(15) });

    checkHeadRowIs(mc, id, "6");
}

TEST_CASE("Terminal.blocks.anEraseThatTakesTheHeadRowReheadsTheBlockWithoutAReflow",
          "[terminal][semanticblocks]")
{
    // What reads a head as it finds it -- the sticky header asking whether a block's head is above the view
    // -- must not be handed a row an erase took out of the block. No reflow renames the rows here, so the
    // stable-id generation does not move; the next read re-heads the block all the same.
    auto mc = MockTerm { PageSize { LineCount(12), ColumnCount(20) }, LineCount(50) };
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();

    auto const id = runEraseAbove(mc);

    REQUIRE(grid.stableIdGeneration() == generation);
    checkHeadRowIs(mc, id, "6");
}

TEST_CASE("Terminal.blocks.aHeadThatScrolledOffStaysEvictedThroughAnErase", "[terminal][semanticblocks]")
{
    // With no history kept, the prompt scrolls off the page and is gone. The command then erases the page
    // above the cursor and prints on, so the rows the block still holds lie below rows that carry no block,
    // not at the top. Within one generation the head's stable id says what became of it: below the floor, it
    // scrolled out, and the block stays evicted rather than take its topmost surviving row.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(0) };
    typeAtPrompt(mc, "seq");
    mc.writeToScreen("\033]133;C\033\\");
    for (auto const i: std::views::iota(1, 9))
        mc.writeToScreen(std::format("{}\r\n", i));
    auto const id = mc.terminal.commandBlocks().currentId();
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    REQUIRE(mc.terminal.commandBlocks().find(id)->headStableId < grid.stableRangeFloor());

    mc.writeToScreen("\033[1J9\r\n");
    REQUIRE(grid.lineAt(LineOffset(0)).blockId() == CommandBlockId {});
    REQUIRE(grid.lineTextTrimmed(LineOffset(4)) == "9");

    auto const* record = mc.terminal.commandBlocks().find(id);
    REQUIRE(record != nullptr);
    CHECK(grid.stableIdGeneration() == generation);
    CHECK(record->headStableId < grid.stableRangeFloor());
}

TEST_CASE("Terminal.blocks.aHeadErasedBelowTheFirstRowTheGridHoldsIsReheadedThere",
          "[terminal][semanticblocks]")
{
    // `printf '\e[H\e[J'; echo top` in a terminal that keeps no history: the cursor homes onto the page's top
    // row, which belongs to no block and so takes the running one, and the ED 0 erases the prompt and the
    // output start below it. The block's topmost row is now the first the grid holds -- where, after a
    // reflow, a head that scrolled out looks no different -- but within one generation the head's stable id
    // is still in range: erased, not evicted, and the block is headed at that row.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(0) };
    mc.writeToScreen("before\r\n");
    typeAtPrompt(mc, "clear");
    mc.writeToScreen("\033]133;C\033\\\033[H\033[Jtop\r\n");
    auto const id = mc.terminal.commandBlocks().currentId();
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.addressableTop() == LineOffset(0));
    REQUIRE(grid.lineAt(LineOffset(0)).blockId() == id);

    checkHeadRowIs(mc, id, "top");
}

TEST_CASE("Terminal.blocks.aPromptARegionAtThePageTopScrollsAwayKeepsItsHead", "[terminal][semanticblocks]")
{
    // apt keeps its progress line below a scroll region that starts at the page's top row, and prints into
    // the region. Such a region scrolls the rows leaving its top into the history with their stable ids, as a
    // whole-page scroll does: the prompt the region carries along keeps its head.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) }, LineCount(20) };
    typeAtPrompt(mc, "apt");
    mc.writeToScreen("\033]133;C\033\\\033[1;7r\033[8;1HProgress: 0%\033[7;1H");
    auto const id = mc.terminal.commandBlocks().currentId();
    for (auto const i: std::views::iota(1, 11))
        mc.writeToScreen(std::format("Get:{}\r\n", i));
    REQUIRE(mc.terminal.primaryScreen().grid().lineTextTrimmed(LineOffset(7)) == "Progress: 0%");

    checkHeadIs(mc, id, LineFlag::Marked, "$ apt");
}

TEST_CASE("Terminal.blocks.aPromptAScrollInsideMarginsMovesKeepsItsHead", "[terminal][semanticblocks]")
{
    // A scroll inside margins that do not start at the page's top -- a region below a status line, IL, DL --
    // moves the rows' contents while each stable id keeps naming its place. The head must follow the prompt,
    // though the row its old id names may now hold a line that looks like a head too: the output start's,
    // which carries the block's id and OutputStart.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) }, LineCount(20) };
    mc.writeToScreen("status\r\nlog1\r\nlog2\r\n");
    typeAtPrompt(mc, "make");
    mc.writeToScreen("\033]133;C\033\\out1\r\n");
    auto const id = mc.terminal.commandBlocks().currentId();
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    REQUIRE(grid.lineTextTrimmed(LineOffset(3)) == "$ make");

    auto promptRow = LineOffset {};
    SECTION("a region below a status line scrolls up")
    {
        mc.writeToScreen("\033[2;8r\033[8;1Hx\r\n");
        promptRow = LineOffset(2);
    }
    SECTION("a line above the prompt is deleted (DL)")
    {
        mc.writeToScreen("\033[2;1H\033[M");
        promptRow = LineOffset(2);
    }
    SECTION("a line above the prompt is inserted (IL)")
    {
        mc.writeToScreen("\033[2;1H\033[L");
        promptRow = LineOffset(4);
    }
    REQUIRE(grid.stableIdGeneration() == generation);
    REQUIRE(grid.lineTextTrimmed(promptRow) == "$ make");

    checkHeadIs(mc, id, LineFlag::Marked, "$ make");
}

TEST_CASE("Terminal.blocks.aHeadAReverseScrollDropsIsReheaded", "[terminal][semanticblocks]")
{
    // SD scrolls the whole page down: the rows' contents move from id to id, and the bottom row's is dropped.
    // The block whose prompt was there keeps only the row the cursor, still on the last row, hands it -- and
    // is headed there.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(20) };
    for (auto const i: std::views::iota(0, 6))
        mc.writeToScreen(std::format("{}\r\n", i));
    mc.writeToScreen("\033]133;A\033\\$ x\033]133;C\033\\");
    auto const id = mc.terminal.commandBlocks().currentId();
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();

    mc.writeToScreen("\033[T");

    REQUIRE(grid.stableIdGeneration() == generation);
    REQUIRE(grid.lineTextTrimmed(LineOffset(3)) == "5");
    checkHeadRowIs(mc, id, "5");
}

TEST_CASE("Terminal.blocks.aReverseScrollKeepsTheBlocksInTheHistory", "[terminal][semanticblocks]")
{
    // SD scrolls the page and nothing above it. The blocks whose rows lie in the history keep every one of
    // them, their heads included, even where the newest history row is a block's prompt; the rows on the page
    // carry their block down with them.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(20) };
    runCommand(mc, "ls", { "a" });
    auto const ls = mc.terminal.commandBlocks().currentId();
    runCommand(mc, "pwd", { "/", "x", "y" });
    auto const pwd = mc.terminal.commandBlocks().currentId();
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    REQUIRE(grid.historyLineCount() == LineCount(3));
    REQUIRE(grid.lineTextTrimmed(LineOffset(-1)) == "$ pwd");

    mc.writeToScreen("\033[2T");

    REQUIRE(grid.stableIdGeneration() == generation);
    auto const blockOf = [&](int row) {
        return grid.lineAt(LineOffset(row)).blockId();
    };
    CHECK(blockOf(-3) == ls);
    CHECK(blockOf(-2) == ls);
    CHECK(blockOf(-1) == pwd);
    CHECK(grid.lineTextTrimmed(LineOffset(2)) == "/");
    CHECK(blockOf(2) == pwd);
    CHECK(blockOf(3) == pwd);
    checkHeadIs(mc, ls, LineFlag::Marked, "$ ls");
    checkHeadIs(mc, pwd, LineFlag::Marked, "$ pwd");
}

TEST_CASE("Terminal.blocks.aPromptDrawnOnAnOlderPromptsRowReheadsTheOlderBlock", "[terminal][semanticblocks]")
{
    // A new ;A on a row an older block's prompt still holds hands that whole logical line to the new block,
    // the older block's head with it. The older block is headed afresh by the rule -- or evicted when nothing
    // of it is left -- and never keeps a head on a row of the new one.
    auto mc = MockTerm { PageSize { LineCount(8), ColumnCount(20) }, LineCount(20) };
    auto const& grid = mc.terminal.primaryScreen().grid();

    SECTION("its output start is left")
    {
        runCommand(mc, "ls", { "a" });
        auto const older = mc.terminal.commandBlocks().currentId();
        mc.writeToScreen("\033[H\033]133;A\033\\$ ");
        REQUIRE(mc.terminal.commandBlocks().currentId() != older);
        REQUIRE(grid.lineAt(LineOffset(0)).blockId() == mc.terminal.commandBlocks().currentId());

        checkHeadIs(mc, older, LineFlag::OutputStart, "a");
        CHECK(grid.lineAt(LineOffset(1)).blockId() == older);
    }

    SECTION("nothing of it is left")
    {
        // A command whose output start and end share the prompt's line, as `printf x` gives at a shell that
        // sends ;C before running it.
        mc.writeToScreen("\033]133;A\033\\$ \033]133;C\033\\x\033]133;D;0\033\\");
        auto const older = mc.terminal.commandBlocks().currentId();
        auto const generation = grid.stableIdGeneration();
        mc.writeToScreen("\r\033]133;A\033\\$ ");
        REQUIRE(mc.terminal.commandBlocks().currentId() != older);

        auto const* record = mc.terminal.commandBlocks().find(older);
        REQUIRE(record != nullptr);
        CHECK(record->headIdGeneration == generation);
        CHECK(record->headStableId < grid.stableRangeFloor());
    }
}

TEST_CASE("Terminal.blocks.aRowTheCursorVisitedAboveThePromptLeavesTheHeadInPlace",
          "[terminal][semanticblocks]")
{
    // A row the cursor reaches that belongs to no block adopts the running one, so the row `clear` homes
    // onto -- above its prompt, written before any shell integration -- is now the block's topmost row. The
    // prompt itself survives, and the resize must find it rather than call the head evicted.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    mc.writeToScreen(std::string(30, 'x') + "\r\n"); // a wrapped line, so the resize rebuilds the ring
    auto const id = runClear(mc);
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.lineAt(HomedRowAfterClear).blockId() == id);
    REQUIRE_FALSE(grid.lineAt(HomedRowAfterClear).isFlagEnabled(LineFlag::Marked));

    reflowTo(mc, PageSize { LineCount(6), ColumnCount(12) });

    checkHeadIs(mc, id, LineFlag::Marked, "$ clear");
}

TEST_CASE("Terminal.blocks.anOlderPromptErasedUnderTheCursorDoesNotBecomeTheHead",
          "[terminal][semanticblocks]")
{
    // The same, when the row `clear` homes onto is an older prompt, and the program erases it before the
    // page goes: an erase starts the row afresh under the running block but keeps its marks, so it now
    // carries this block's id AND the older prompt's mark, above this block's own prompt.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    runCommand(mc, "ls", { std::string(30, 'a') });
    auto const id = runClear(mc, "\033[2K");
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.lineAt(HomedRowAfterClear).blockId() == id);
    REQUIRE(grid.lineAt(HomedRowAfterClear).isFlagEnabled(LineFlag::Marked));
    REQUIRE(grid.lineTextTrimmed(HomedRowAfterClear).empty());

    reflowTo(mc, PageSize { LineCount(6), ColumnCount(12) });

    checkHeadIs(mc, id, LineFlag::Marked, "$ clear");
}

TEST_CASE("Terminal.blocks.headsAreReRecordedOnlyWhenRowIdentityDies", "[terminal][semanticblocks]")
{
    // The rescan is keyed on the grid's stable-id generation, not on what a record holds: a record adopted
    // with a head from a generation the grid is not in keeps it until row identity next dies, and is
    // re-recorded then like any other.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(50) };
    mc.writeToScreen(std::string(30, 'x') + "\r\n"); // a wrapped line, so the resize rebuilds the ring
    mc.writeToScreen("\033]133;A\033\\$ ");
    REQUIRE(mc.terminal.commandBlocks().current() != nullptr);
    auto record = *mc.terminal.commandBlocks().current();
    record.headIdGeneration = UnknownHeadGeneration;
    CHECK_FALSE(mc.terminal.commandBlocks().adopt(record, AdoptMode::Snapshot).has_value());

    auto const* adopted = mc.terminal.commandBlocks().find(record.id);
    REQUIRE(adopted != nullptr);
    CHECK(adopted->headIdGeneration == UnknownHeadGeneration);

    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(12) });
    checkHeadIs(mc, record.id, LineFlag::Marked, "$");
}

// }}}

// {{{ history-limit changes

TEST_CASE("Terminal.blocks.aHistoryLimitChangeKeepsThePrimaryCursorLineCurrent", "[terminal][semanticblocks]")
{
    // A config reload raises the history limit while a full-screen program has the alternate screen up. The
    // primary grid's ring is turned back to its zero and regrown, which moves every row; the primary screen's
    // cached cursor line must follow, because each batch stamps the primary page's cursor row even while
    // another page is current.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(10) }, LineCount(10) };
    for (auto const i: std::views::iota(0, 12))
        mc.writeToScreen(std::format("{}\r\n", i)); // the ring has turned
    mc.writeToScreen("\033[?1049h");

    mc.terminal.setHistoryLimits(HistoryLimits::plain(LineCount(500)));

    auto& primary = mc.terminal.primaryScreen();
    CHECK(&primary.currentLine() == &primary.grid().lineAt(primary.cursor().position.line));
}

TEST_CASE("Terminal.blocks.aBatchAfterAHistoryLimitChangeStampsThePrimaryCursorRow",
          "[terminal][semanticblocks]")
{
    // The same change, with the primary cursor on a row no batch has seen yet: a reset the GUI asked for put
    // it there, between batches. The next batch -- written to the alternate screen -- stamps that row.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(10) }, LineCount(10) };
    mc.wallClock.advance(5s);
    mc.writeToScreen("one\r\ntwo");
    core::locked(mc.terminal, [&]() {
        mc.terminal.hardReset();
        mc.terminal.setScreen(ScreenType::Alternate);
    });
    REQUIRE(bornAtRow(mc, 0) == 0);

    mc.terminal.setHistoryLimits(HistoryLimits::plain(LineCount(500)));
    mc.wallClock.advance(10s);
    mc.writeToScreen("x");

    CHECK(bornAtRow(mc, 0) == 16);
}

TEST_CASE("Terminal.blocks.anUnchangedHistoryLimitKeepsRowIdentity", "[terminal][semanticblocks]")
{
    // Every config reload hands the terminal its history limits again, almost always unchanged. That is no
    // rebuild of the ring, and must not count as one: every block's head would be re-synced, and every fold
    // reset, on every reload.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(10) }, LineCount(10) };
    runCommand(mc, "ls", { "a", "b", "c", "d" });
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    auto const revision = mc.terminal.commandBlocks().revision();

    mc.terminal.setHistoryLimits(HistoryLimits::plain(LineCount(10)));

    CHECK(grid.stableIdGeneration() == generation);
    CHECK(mc.terminal.commandBlocks().revision() == revision);
}

// }}}
