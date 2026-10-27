// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/SemanticBlockTracker.hpp>
#include <vtbackend/testing/TestHelpers.hpp>
#include <vtbackend/vt/Sequence.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;
using namespace std::string_view_literals;
using vtbackend::test::mirroredFinish;
using vtbackend::test::mirroredRecord;
using vtbackend::test::recordIds;

// {{{ the vocabulary

TEST_CASE("CommandBlock.truncateUtf8.keepsWhatFits", "[semanticblocks][blockstore]")
{
    CHECK(truncateUtf8("hello", 5) == "hello");
    CHECK(truncateUtf8("hello", 64) == "hello");
    CHECK(truncateUtf8("", 0).empty());
    // Fitting wins over the ellipsis budget: nothing is cut, so nothing is appended.
    CHECK(truncateUtf8("ab", 2) == "ab");
}

TEST_CASE("CommandBlock.truncateUtf8.theEllipsisCountsAgainstTheBudget", "[semanticblocks][blockstore]")
{
    // Eight ASCII bytes into six: three kept, plus the three-byte "…".
    CHECK(truncateUtf8("abcdefgh", 6) == "abc\xE2\x80\xA6");
    CHECK(truncateUtf8("abcdefgh", 3) == "\xE2\x80\xA6");
}

TEST_CASE("CommandBlock.truncateUtf8.neverSplitsACharacter", "[semanticblocks][blockstore]")
{
    // Each text is "a", one multi-byte character, then "bcdef". A budget of n leaves n - 3 bytes before
    // the ellipsis, so budget 4 cuts at the character's lead byte and each budget after it at the next
    // continuation byte: every one of them must drop the whole character, which takes the back-off up
    // to three steps. The budget after the last continuation byte cuts just past the character, and
    // keeps it.
    auto const ellipsis = std::string { "\xE2\x80\xA6" };
    auto const twoBytes = std::string { "\xC3\xA9" };          // U+00E9
    auto const threeBytes = std::string { "\xE2\x82\xAC" };    // U+20AC
    auto const fourBytes = std::string { "\xF0\x9F\x98\x80" }; // U+1F600
    auto const around = [](std::string const& character) {
        return "a" + character + "bcdef";
    };

    struct Row
    {
        std::string_view name;
        std::string text;
        size_t budget;
        std::string expected;
    };

    auto const rows = std::array {
        Row { "two bytes, cut at the lead byte", around(twoBytes), 4, "a" + ellipsis },
        Row { "two bytes, cut at +1", around(twoBytes), 5, "a" + ellipsis },
        Row { "two bytes, cut past it", around(twoBytes), 6, "a" + twoBytes + ellipsis },
        Row { "three bytes, cut at the lead byte", around(threeBytes), 4, "a" + ellipsis },
        Row { "three bytes, cut at +1", around(threeBytes), 5, "a" + ellipsis },
        Row { "three bytes, cut at +2", around(threeBytes), 6, "a" + ellipsis },
        Row { "three bytes, cut past it", around(threeBytes), 7, "a" + threeBytes + ellipsis },
        Row { "four bytes, cut at the lead byte", around(fourBytes), 4, "a" + ellipsis },
        Row { "four bytes, cut at +1", around(fourBytes), 5, "a" + ellipsis },
        Row { "four bytes, cut at +2", around(fourBytes), 6, "a" + ellipsis },
        Row { "four bytes, cut at +3", around(fourBytes), 7, "a" + ellipsis },
        Row { "four bytes, cut past it", around(fourBytes), 8, "a" + fourBytes + ellipsis },
    };

    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const cut = truncateUtf8(row.text, row.budget);
        CHECK(cut == row.expected);
        CHECK(cut.size() <= row.budget);
    }
}

TEST_CASE("CommandBlock.truncateUtf8.aBudgetBelowTheEllipsisYieldsNothing", "[semanticblocks][blockstore]")
{
    CHECK(truncateUtf8("abcdef", 2).empty());
    CHECK(truncateUtf8("abcdef", 0).empty());
}

TEST_CASE("CommandBlock.truncatedCommandLine.keepsWhatFits", "[semanticblocks][blockstore]")
{
    CHECK(truncatedCommandLine("ls -la") == "ls -la");
    auto const exactly = std::string(MaxRecordedCommandLineBytes, 'x');
    CHECK(truncatedCommandLine(exactly) == exactly);
}

TEST_CASE("CommandBlock.isTruncatedCommandLine.recognisesWhatTruncatedCommandLineMakes",
          "[semanticblocks][blockstore]")
{
    // Every cut position a multi-byte code point can give, and the plain one.
    for (auto const* const unit: { "x", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80" })
    {
        auto line = std::string {};
        while (line.size() < MaxRecordedCommandLineBytes + 8)
            line += unit;
        CHECK(isTruncatedCommandLine(truncatedCommandLine(line)));
    }
    CHECK_FALSE(isTruncatedCommandLine("ls -la"));
    CHECK_FALSE(isTruncatedCommandLine(std::string(MaxRecordedCommandLineBytes, 'x')));
}

TEST_CASE("CommandBlock.truncatedCommandLine.cutsAtACodepointAndEndsInAnEllipsis",
          "[semanticblocks][blockstore]")
{
    auto const ellipsis = "\xE2\x80\xA6"sv;

    SECTION("ASCII is cut at the byte budget")
    {
        auto const cut = truncatedCommandLine(std::string(MaxRecordedCommandLineBytes + 1, 'x'));
        CHECK(cut.size() == MaxRecordedCommandLineBytes);
        CHECK(cut.ends_with(ellipsis));
        CHECK(cut.substr(0, cut.size() - ellipsis.size())
              == std::string(MaxRecordedCommandLineBytes - ellipsis.size(), 'x'));
    }

    SECTION("a three-byte character is never split")
    {
        auto text = std::string {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, 2000))
            text += "\xE2\x82\xAC"; // €
        auto const cut = truncatedCommandLine(text);
        REQUIRE(cut.ends_with(ellipsis));
        auto const kept = std::string_view { cut }.substr(0, cut.size() - ellipsis.size());
        CHECK(cut.size() <= MaxRecordedCommandLineBytes);
        CHECK(kept.size() % 3 == 0);
        CHECK(kept == std::string_view { text }.substr(0, kept.size()));
    }

    SECTION("a four-byte character is never split")
    {
        auto text = std::string {};
        for ([[maybe_unused]] auto const _: std::views::iota(0, 1500))
            text += "\xF0\x9F\x98\x80"; // U+1F600
        auto const cut = truncatedCommandLine(text);
        REQUIRE(cut.ends_with(ellipsis));
        auto const kept = std::string_view { cut }.substr(0, cut.size() - ellipsis.size());
        CHECK(cut.size() <= MaxRecordedCommandLineBytes);
        CHECK(kept.size() % 4 == 0);
        CHECK(kept == std::string_view { text }.substr(0, kept.size()));
    }

    SECTION("bytes that are not UTF-8 are cut after at most three steps back")
    {
        // Every byte continues a sequence that never started. The back-off gives up after the three
        // steps the longest sequence (four bytes) could need, and cuts there.
        constexpr auto MaxBackOff = size_t { 3 };
        auto const garbage = std::string(MaxRecordedCommandLineBytes * 2, '\x80');
        auto const cut = truncatedCommandLine(garbage);
        CHECK(cut.size() == MaxRecordedCommandLineBytes - MaxBackOff);
        CHECK(cut
              == garbage.substr(0, MaxRecordedCommandLineBytes - ellipsis.size() - MaxBackOff)
                     + std::string { ellipsis });
    }
}

TEST_CASE("CommandBlock.outcomeOf", "[semanticblocks][blockstore]")
{
    struct Row
    {
        std::string_view name;
        CommandBlockState state;
        std::optional<int> exitCode;
        ContextOutcome outcome;
        CommandBlockOutcome expected;
    };

    auto const rows = std::array {
        Row { "running", CommandBlockState::Running, std::nullopt, {}, CommandBlockOutcome::Running },
        Row { "prompting", CommandBlockState::Prompting, std::nullopt, {}, CommandBlockOutcome::Success },
        Row { "exit 0", CommandBlockState::Finished, 0, {}, CommandBlockOutcome::Success },
        Row { "exit 2", CommandBlockState::Finished, 2, {}, CommandBlockOutcome::Failure },
        Row { "implicit, no code",
              CommandBlockState::Finished,
              std::nullopt,
              {},
              CommandBlockOutcome::Success },
        Row { "exit 0 but killed",
              CommandBlockState::Finished,
              0,
              ContextOutcome { .exit = ContextExit::Crash, .signal = ContextSignal::Segv },
              CommandBlockOutcome::Failure },
        Row { "interrupted",
              CommandBlockState::Finished,
              std::nullopt,
              ContextOutcome { .exit = ContextExit::Interrupt },
              CommandBlockOutcome::Failure },
        Row { "a signal alone",
              CommandBlockState::Finished,
              std::nullopt,
              ContextOutcome { .signal = ContextSignal::Kill },
              CommandBlockOutcome::Failure },
        Row { "3008 failure, no code",
              CommandBlockState::Finished,
              std::nullopt,
              ContextOutcome { .exit = ContextExit::Failure, .status = 1 },
              CommandBlockOutcome::Failure },
        // asShellExitCode() reads this as 0; the exit kind still says the command failed.
        Row { "3008 failure, status 0",
              CommandBlockState::Finished,
              0,
              ContextOutcome { .exit = ContextExit::Failure, .status = 0 },
              CommandBlockOutcome::Failure },
        Row { "3008 success",
              CommandBlockState::Finished,
              0,
              ContextOutcome { .exit = ContextExit::Success },
              CommandBlockOutcome::Success },
    };

    for (auto const& row: rows)
    {
        INFO(row.name);
        auto record = CommandBlockRecord {};
        record.state = row.state;
        record.exitCode = row.exitCode;
        record.outcome = row.outcome;
        CHECK(outcomeOf(record) == row.expected);
    }
}

TEST_CASE("CommandBlock.commandEndingOf", "[semanticblocks][blockstore]")
{
    struct Row
    {
        std::string_view name;
        std::optional<int> exitCode;
        ContextOutcome outcome;
        CommandEnding expected;
    };

    auto const rows = std::array {
        Row { "nothing known", std::nullopt, {}, CommandEnding::Unknown },
        Row { "exit 0", 0, {}, CommandEnding::Succeeded },
        Row { "exit 2", 2, {}, CommandEnding::Failed },
        Row { "a shell's 128 + SIGKILL", 137, {}, CommandEnding::Killed },
        Row { "128 + a signal numbered differently elsewhere", 138, {}, CommandEnding::Failed },
        Row { "3008 success",
              std::nullopt,
              ContextOutcome { .exit = ContextExit::Success },
              CommandEnding::Succeeded },
        Row { "3008 success, but a non-zero code",
              2,
              ContextOutcome { .exit = ContextExit::Success },
              CommandEnding::Failed },
        Row { "3008 failure, no status",
              0,
              ContextOutcome { .exit = ContextExit::Failure },
              CommandEnding::Failed },
        Row { "3008 failure, status 130",
              130,
              ContextOutcome { .exit = ContextExit::Failure, .status = 130 },
              CommandEnding::Failed },
        Row { "3008 crash", 139, ContextOutcome { .exit = ContextExit::Crash }, CommandEnding::Crashed },
        Row { "3008 interrupt",
              130,
              ContextOutcome { .exit = ContextExit::Interrupt },
              CommandEnding::Interrupted },
        Row { "3008 named signal",
              139,
              ContextOutcome { .exit = ContextExit::Crash, .signal = ContextSignal::Segv },
              CommandEnding::Killed },
    };

    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(commandEndingOf(row.exitCode, row.outcome) == row.expected);
    }
}

TEST_CASE("CommandBlock.outcomeOf and the finish classification agree on what failed",
          "[semanticblocks][blockstore]")
{
    // Every exit code shape against every OSC 3008 verdict. The rule, spelled out rather than re-derived:
    // a finished command failed if either source says so.
    auto const exitCode = GENERATE(std::optional<int> {},
                                   std::optional<int> { 0 },
                                   std::optional<int> { 2 },
                                   std::optional<int> { 130 },
                                   std::optional<int> { 138 },
                                   std::optional<int> { 255 });
    auto const exit = GENERATE(ContextExit::Unknown,
                               ContextExit::Success,
                               ContextExit::Failure,
                               ContextExit::Crash,
                               ContextExit::Interrupt);
    auto const signal = GENERATE(ContextSignal::None, ContextSignal::Segv);
    auto const status = GENERATE(uint64_t { 0 }, uint64_t { 130 }, uint64_t { 256 });

    auto record = CommandBlockRecord {};
    record.state = CommandBlockState::Finished;
    record.exitCode = exitCode;
    record.outcome = ContextOutcome { .exit = exit, .signal = signal, .status = status };

    auto const failedByCode = exitCode.has_value() && *exitCode != 0;
    auto const failedByOutcome = signal != ContextSignal::None || exit == ContextExit::Failure
                                 || exit == ContextExit::Crash || exit == ContextExit::Interrupt;
    auto const expected =
        (failedByCode || failedByOutcome) ? CommandBlockOutcome::Failure : CommandBlockOutcome::Success;

    INFO(std::format("exit code {} exit {} signal {} status {}",
                     exitCode.value_or(-1),
                     static_cast<int>(exit),
                     static_cast<int>(signal),
                     status));
    CHECK(outcomeOf(record) == expected);
    CHECK(isFailure(commandEndingOf(record.exitCode, record.outcome))
          == (expected == CommandBlockOutcome::Failure));
}

TEST_CASE("CommandBlock.summaryOf", "[semanticblocks][blockstore]")
{
    auto record = CommandBlockRecord {};
    record.id = CommandBlockId { 3 };
    record.state = CommandBlockState::Finished;
    record.commandLine = vtbackend::CommandLineText { "make" };
    record.exitCode = 2;
    record.outcome = ContextOutcome { .exit = ContextExit::Failure, .status = 2 };
    record.duration = 4s;
    record.workingDirectory = WorkingDirectorySnapshot { .path = "/src", .locality = ContextLocality::Local };

    CHECK(summaryOf(record)
          == CommandBlockSummary { .id = CommandBlockId { 3 },
                                   .commandLine = CommandLineText { "make" },
                                   .exitCode = 2,
                                   .outcome = ContextOutcome { .exit = ContextExit::Failure, .status = 2 },
                                   .duration = 4s,
                                   .workingDirectory = record.workingDirectory });

    // A block closed implicitly has no duration; the summary says so rather than inventing one -- not
    // even zero, which the finish notification would print as "took 0ms".
    record.duration.reset();
    CHECK_FALSE(summaryOf(record).duration.has_value());
}

// }}}

// {{{ the store

namespace
{

/// A store on manual clocks: time moves only when the test says so.
struct StoreFixture
{
    core::platform::ManualClock steady;
    core::platform::ManualWallClock wall { std::chrono::system_clock::time_point {
        std::chrono::hours { 24 } } };

    /// Every change the store told its listener of, in order.
    std::vector<CommandBlockChange> changes;

    CommandBlockStore store;

    explicit StoreFixture(CommandBlockStoreLimits limits = {}):
        store {
            limits, steady, wall, [this](CommandBlockChange const& change) { changes.push_back(change); }
        },
        _seenRevision { store.revision() }
    {
    }

    /// @return The records the changes told since the last call named, or since the fixture was built.
    [[nodiscard]] std::vector<CommandBlockId> takeChanges()
    {
        return std::ranges::to<std::vector>(std::exchange(changes, {})
                                            | std::views::transform(&CommandBlockChange::id));
    }

    /// @return Whether the store's revision moved since the last call, or since the fixture was built.
    [[nodiscard]] bool revisionMoved()
    {
        auto const now = store.revision();
        auto const moved = now != _seenRevision;
        _seenRevision = now;
        return moved;
    }

    /// Runs one whole cycle: a prompt, its command, and a finish with exit code 0.
    /// @param headStableId Where the prompt's head row is.
    /// @return The id of the block the cycle ran in.
    CommandBlockId runCycle(int64_t headStableId)
    {
        auto const id = store.promptStarted(headStableId, 0);
        store.commandStarted(CommandStart {});
        std::ignore = store.commandFinished(0);
        return id;
    }

  private:
    uint64_t _seenRevision;
};

/// An OSC 133;C that carries a cmdline_url and nothing else.
[[nodiscard]] CommandStart reported(std::string commandLine)
{
    return CommandStart { .commandLine = std::move(commandLine), .source = CommandLineSource::Reported };
}

// The steady clock is borrowed, so a temporary one -- which would dangle -- is refused at compile time.
static_assert(!std::is_constructible_v<CommandBlockStore,
                                       CommandBlockStoreLimits,
                                       core::platform::SteadyClock,
                                       core::platform::WallClockRef,
                                       CommandBlockChangeListener>);
static_assert(std::is_constructible_v<CommandBlockStore,
                                      CommandBlockStoreLimits,
                                      core::platform::SteadyClock const&,
                                      core::platform::WallClockRef,
                                      CommandBlockChangeListener>);

} // namespace

TEST_CASE("CommandBlockStore.aPromptMintsAPromptingRecord", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const id = f.store.promptStarted(10, 3);

    CHECK(id == CommandBlockId { 1 });
    REQUIRE(f.store.size() == 1);
    auto const* record = f.store.current();
    REQUIRE(record != nullptr);
    CHECK(record->id == id);
    CHECK(record->state == CommandBlockState::Prompting);
    CHECK(record->promptStartedAt == f.wall.now());
    CHECK(record->headStableId == 10);
    CHECK(record->headIdGeneration == 3);
    CHECK(f.store.currentId() == id);
    CHECK(f.store.lastFinished() == nullptr);
}

TEST_CASE("CommandBlockStore.theLastCommandIsTheNewestRecordThatRanOne", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    CHECK(f.store.lastCommand() == nullptr);

    auto const first = f.store.promptStarted(0, 0);
    CHECK(f.store.lastCommand() == nullptr); // a prompt alone runs nothing

    f.store.commandStarted(CommandStart { .commandLine = "make", .source = CommandLineSource::Reported });
    REQUIRE(f.store.lastCommand() != nullptr);
    CHECK(f.store.lastCommand()->id == first);

    REQUIRE(f.store.commandFinished(0).has_value());
    auto const second = f.store.promptStarted(5, 0);
    REQUIRE(f.store.lastCommand() != nullptr);
    CHECK(f.store.lastCommand()->id == first); // finished, with a newer prompt open below it

    f.store.commandStarted(CommandStart { .commandLine = "ls", .source = CommandLineSource::Reported });
    REQUIRE(f.store.lastCommand() != nullptr);
    CHECK(f.store.lastCommand()->id == second);
}

TEST_CASE("CommandBlockStore.aFullCycleFinishesWithExitCodeDurationAndCommandLine",
          "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const id = f.store.promptStarted(0, 0);
    f.wall.advance(2s);
    f.steady.advance(2s);
    auto const startedAt = f.wall.now();
    f.store.commandStarted(
        CommandStart { .commandLine = "make -j8",
                       .source = CommandLineSource::Reported,
                       .workingDirectory = { .path = "/src", .locality = ContextLocality::Local } });
    f.steady.advance(3s);
    f.wall.advance(3s);
    auto const summary = f.store.commandFinished(2);

    REQUIRE(summary.has_value());
    CHECK(summary->id == id);
    CHECK(summary->commandLine.raw() == "make -j8");
    CHECK(summary->exitCode == 2);
    CHECK(summary->duration == 3s);
    CHECK(summary->workingDirectory.path == "/src");

    auto const* record = f.store.find(id);
    REQUIRE(record != nullptr);
    CHECK(record->state == CommandBlockState::Finished);
    CHECK(record->end == CommandBlockEnd::Reported);
    CHECK(record->commandStartedAt == startedAt);
    CHECK(record->duration == 3s);
    CHECK(record->workingDirectory.locality == ContextLocality::Local);
    CHECK(f.store.current() == nullptr);
    CHECK(f.store.lastFinished() == record);
    // Rows the cursor reaches after the ;D still belong to it, until the next prompt starts.
    CHECK(f.store.currentId() == id);
}

TEST_CASE("CommandBlockStore.aPromptThatRanNothingIsDiscardedByTheNext", "[semanticblocks][blockstore]")
{
    // An empty Enter, or Ctrl-C at the prompt: the shell draws a fresh prompt and never emits ;C.
    auto f = StoreFixture {};
    auto const first = f.store.promptStarted(0, 0);
    auto const second = f.store.promptStarted(1, 0);

    CHECK(f.store.size() == 1);
    CHECK(f.store.find(first) == nullptr);
    CHECK(f.store.find(second) != nullptr);
    CHECK(second != first);
}

TEST_CASE("CommandBlockStore.aNewPromptClosesARunningBlockImplicitly", "[semanticblocks][blockstore]")
{
    // `ssh host` runs and the REMOTE shell's prompt arrives: the outer command has not finished, so its
    // block is closed without an exit code -- and promptStarted() returns no summary to announce.
    auto f = StoreFixture {};
    auto const outer = f.store.promptStarted(0, 0);
    f.store.commandStarted(reported("ssh host"));
    auto const inner = f.store.promptStarted(5, 0);

    auto const* closed = f.store.find(outer);
    REQUIRE(closed != nullptr);
    CHECK(closed->state == CommandBlockState::Finished);
    CHECK(closed->end == CommandBlockEnd::Implicit);
    CHECK_FALSE(closed->exitCode.has_value());
    CHECK_FALSE(closed->duration.has_value());
    REQUIRE(f.store.current() != nullptr);
    CHECK(f.store.current()->id == inner);
    CHECK(f.store.current()->state == CommandBlockState::Prompting);
    // The ;D the remote shell's precmd sends belongs to no command this store saw start.
    CHECK_FALSE(f.store.commandFinished(0).has_value());
}

TEST_CASE("CommandBlockStore.aCommandStartWithoutAPromptMintsItsOwnRecord", "[semanticblocks][blockstore]")
{
    // A shell integration sourced halfway through a session: its first sequence may well be a ;C.
    auto f = StoreFixture {};
    f.store.commandStarted(CommandStart { .headStableId = 42, .headIdGeneration = 7 });

    auto const* record = f.store.current();
    REQUIRE(record != nullptr);
    CHECK(record->state == CommandBlockState::Running);
    CHECK(record->headStableId == 42);
    CHECK(record->headIdGeneration == 7);
    CHECK(record->commandStartedAt == f.wall.now());
    // No prompt was marked, so none started: what tells this block from one whose prompt row left.
    CHECK_FALSE(record->promptStartedAt.has_value());
    CHECK(record->commandLineSource == CommandLineSource::None);
}

TEST_CASE("CommandBlockStore.aCommandStartAfterAFinishMintsAFreshRecord", "[semanticblocks][blockstore]")
{
    // A prompt theme that replaces PS1 drops the ;A, while the shell's preexec and precmd hooks keep sending
    // ;C and ;D: the next ;C then follows the previous ;D directly.
    auto f = StoreFixture {};
    auto const first = f.store.promptStarted(0, 0);
    f.store.commandStarted(reported("ls"));
    std::ignore = f.store.commandFinished(0);
    f.store.commandStarted(CommandStart { .commandLine = "make",
                                          .source = CommandLineSource::Reported,
                                          .headStableId = 9,
                                          .headIdGeneration = 1 });

    auto const* running = f.store.current();
    REQUIRE(running != nullptr);
    CHECK(running->id > first);
    CHECK(running->state == CommandBlockState::Running);
    CHECK(running->commandLine.raw() == "make");
    CHECK(running->headStableId == 9);
    CHECK(running->headIdGeneration == 1);
    CHECK(f.store.currentId() == running->id);

    auto const* previous = f.store.find(first);
    REQUIRE(previous != nullptr);
    CHECK(previous->state == CommandBlockState::Finished);
    CHECK(previous->end == CommandBlockEnd::Reported);
    CHECK(previous->commandLine.raw() == "ls");
    CHECK(f.store.lastFinished() == previous);
    CHECK(f.store.size() == 2);
}

TEST_CASE("CommandBlockStore.aFinishWithNothingRunningIsIgnored", "[semanticblocks][blockstore]")
{
    // tcsh emits ;D unconditionally; a nested shell's exit leaves one behind at the outer prompt.
    auto f = StoreFixture {};
    CHECK_FALSE(f.store.commandFinished(1).has_value());
    CHECK(f.store.size() == 0);

    f.store.promptStarted(0, 0);
    auto const revision = f.store.revision();
    CHECK_FALSE(f.store.commandFinished(0).has_value());
    REQUIRE(f.store.current() != nullptr);
    CHECK(f.store.current()->state == CommandBlockState::Prompting);
    CHECK(f.store.revision() == revision);
}

TEST_CASE("CommandBlockStore.aRepeatedCommandStartFillsButNeverReplacesTheCommandLine",
          "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    f.store.promptStarted(0, 0);
    f.store.commandStarted(CommandStart {});
    REQUIRE(f.store.current() != nullptr);
    CHECK(f.store.current()->commandLineSource == CommandLineSource::None);

    f.store.commandStarted(reported("ls"));
    CHECK(f.store.current()->commandLine.raw() == "ls");
    CHECK(f.store.current()->commandLineSource == CommandLineSource::Reported);

    f.store.commandStarted(reported("rm -rf /"));
    CHECK(f.store.current()->commandLine.raw() == "ls");
}

TEST_CASE("CommandBlockStore.theWorkingDirectoryIsTheOneInEffectAtTheCommandStart",
          "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    f.store.promptStarted(0, 0);
    f.store.commandStarted(
        CommandStart { .workingDirectory = { .path = "/a", .locality = ContextLocality::Local } });
    f.store.commandStarted(
        CommandStart { .workingDirectory = { .path = "/b", .locality = ContextLocality::Foreign } });

    REQUIRE(f.store.current() != nullptr);
    CHECK(f.store.current()->workingDirectory
          == WorkingDirectorySnapshot { .path = "/a", .locality = ContextLocality::Local });
}

TEST_CASE("CommandBlockStore.theNextPromptRecordsWhereTheCommandLeftTheShell", "[semanticblocks][blockstore]")
{
    // `cd build && make`: the shell reports the directory the command left it in only as the next prompt
    // starts.
    auto f = StoreFixture {};
    auto const src = WorkingDirectorySnapshot { .path = "/src", .locality = ContextLocality::Local };
    auto const build = WorkingDirectorySnapshot { .path = "/src/build", .locality = ContextLocality::Local };
    auto const id = f.store.promptStarted(0, 0, src);
    f.store.commandStarted(CommandStart { .workingDirectory = src });

    SECTION("after the command reported its end")
    {
        std::ignore = f.store.commandFinished(0);
        std::ignore = f.takeChanges();
        f.store.promptStarted(1, 0, build);
        REQUIRE(f.store.find(id) != nullptr);
        CHECK(f.store.find(id)->workingDirectory == src);
        CHECK(f.store.find(id)->endWorkingDirectory == build);
        CHECK(f.takeChanges() == std::vector { id });

        // An empty Enter's prompt discards the one before it, and leaves the command's record as it was.
        f.store.promptStarted(2, 0, src);
        CHECK(f.store.find(id)->endWorkingDirectory == build);
    }

    SECTION("when the next prompt implies its end")
    {
        f.store.promptStarted(1, 0, build);
        REQUIRE(f.store.find(id) != nullptr);
        CHECK(f.store.find(id)->state == CommandBlockState::Finished);
        CHECK(f.store.find(id)->endWorkingDirectory == build);
    }

    SECTION("a path past the bound is dropped, not truncated")
    {
        std::ignore = f.store.commandFinished(0);
        auto const tooLong =
            WorkingDirectorySnapshot { .path = "/" + std::string(MaxRecordedWorkingDirectoryBytes, 'd'),
                                       .locality = ContextLocality::Local };
        f.store.promptStarted(1, 0, tooLong);
        CHECK(f.store.find(id)->endWorkingDirectory == WorkingDirectorySnapshot {});
    }
}

TEST_CASE("CommandBlockStore.aForeignDirectoryMarksTheOpenBlockForGood", "[semanticblocks][blockstore]")
{
    // `ssh host`: the remote shell reports its directory while the local block is open.
    auto f = StoreFixture {};
    auto const ssh = f.store.promptStarted(0, 0);
    f.store.commandStarted(CommandStart {});
    std::ignore = f.takeChanges();
    std::ignore = f.revisionMoved();

    f.store.foreignDirectoryReported();
    CHECK(f.store.find(ssh)->foreignDirectory == ForeignDirectory::Reported);
    CHECK(f.takeChanges() == std::vector { ssh });
    CHECK(f.revisionMoved());

    f.store.foreignDirectoryReported(); // said already
    CHECK(f.takeChanges().empty());
    CHECK_FALSE(f.revisionMoved());

    // The local prompt closes the block, which keeps the mark; the block it opens starts without one.
    auto const next = f.store.promptStarted(1, 0);
    CHECK(f.store.find(ssh)->foreignDirectory == ForeignDirectory::Reported);
    CHECK(f.store.find(next)->foreignDirectory == ForeignDirectory::Unreported);

    // With no block open there is nothing to mark.
    f.store.commandStarted(CommandStart {});
    std::ignore = f.store.commandFinished(0);
    std::ignore = f.takeChanges();
    f.store.foreignDirectoryReported();
    CHECK(f.store.find(next)->foreignDirectory == ForeignDirectory::Unreported);
    CHECK(f.takeChanges().empty());
}

TEST_CASE("CommandBlockStore.aWorkingDirectoryPastTheBoundIsDroppedNotTruncated",
          "[semanticblocks][blockstore]")
{
    // An OSC 7 may be as long as the parser lets any OSC be, and every record keeps its own copy. A cut path
    // would name a different directory, so one that does not fit is not kept at all.
    auto f = StoreFixture {};
    auto const pathOf = [](size_t bytes) {
        return "/" + std::string(bytes - 1, 'd');
    };
    auto const recordedFor = [&](std::string path) {
        f.store.promptStarted(0, 0);
        f.store.commandStarted(CommandStart {
            .workingDirectory = { .path = std::move(path), .locality = ContextLocality::Local } });
        REQUIRE(f.store.current() != nullptr);
        return f.store.current()->workingDirectory;
    };

    CHECK(recordedFor(pathOf(MaxRecordedWorkingDirectoryBytes))
          == WorkingDirectorySnapshot { .path = pathOf(MaxRecordedWorkingDirectoryBytes),
                                        .locality = ContextLocality::Local });
    CHECK(recordedFor(pathOf(MaxRecordedWorkingDirectoryBytes + 1)) == WorkingDirectorySnapshot {});
    CHECK(recordedFor(pathOf(Sequence::MaxOscLength)) == WorkingDirectorySnapshot {});
}

TEST_CASE("CommandBlockStore.aHugeCommandLineIsStoredTruncatedAtACodepoint", "[semanticblocks][blockstore]")
{
    // Whatever wrote to the pty decides how long a cmdline_url is; the store must bound what it keeps.
    auto f = StoreFixture {};
    auto hugeLine = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, (64 * 1024 / 3) + 1))
        hugeLine += "\xE2\x82\xAC"; // €
    f.store.commandStarted(reported(hugeLine));

    auto const* record = f.store.current();
    REQUIRE(record != nullptr);
    CHECK(record->commandLine.raw() == truncatedCommandLine(hugeLine));
}

TEST_CASE("CommandBlockStore.theRevisionMovesWithEveryChangeAndOnlyThen", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};

    f.store.promptStarted(0, 0);
    CHECK(f.revisionMoved());
    f.store.commandStarted(CommandStart {});
    CHECK(f.revisionMoved());
    f.store.commandStarted(CommandStart {}); // still running, still no command line: nothing new
    CHECK_FALSE(f.revisionMoved());
    f.store.commandStarted(reported("ls")); // fills the missing command line
    CHECK(f.revisionMoved());
    f.store.commandStarted(reported("ls")); // never replaces one
    CHECK_FALSE(f.revisionMoved());
    std::ignore = f.store.commandFinished(0);
    CHECK(f.revisionMoved());
    std::ignore = f.store.commandFinished(0); // nothing running: ignored
    CHECK_FALSE(f.revisionMoved());
    f.store.clear();
    CHECK(f.revisionMoved());
    f.store.clear(); // nothing left to forget
    CHECK_FALSE(f.revisionMoved());
    f.store.commandStarted(CommandStart {}); // mints a record
    CHECK(f.revisionMoved());
}

TEST_CASE("CommandBlockStore.clearForgetsEverythingButNeverReusesAnId", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const before = f.store.promptStarted(0, 0);
    f.store.commandStarted(reported("ls"));
    f.store.clear();

    CHECK(f.store.size() == 0);
    CHECK(f.store.current() == nullptr);
    CHECK(f.store.currentId() == CommandBlockId {});
    // A row stamped before the reset must never resolve to a block that came after it.
    CHECK(f.store.promptStarted(0, 0) > before);
}

TEST_CASE("CommandBlockStore.theCapDropsTheOldestFirst", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture { CommandBlockStoreLimits { .maxRecords = 3 } };
    auto ids = std::vector<CommandBlockId> {};
    for (auto const i: std::views::iota(0, 5))
        ids.push_back(f.runCycle(i));

    CHECK(f.store.size() == 3);
    CHECK(f.store.find(ids[0]) == nullptr);
    CHECK(f.store.find(ids[1]) == nullptr);
    CHECK(f.store.find(ids[2]) != nullptr);
    CHECK(f.store.find(ids[4]) != nullptr);
}

TEST_CASE("CommandBlockStore.aCapOfZeroStillKeepsTheCurrentBlock", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture { CommandBlockStoreLimits { .maxRecords = 0 } };
    CHECK(f.store.limits().maxRecords == 1);
    auto const id = f.store.promptStarted(0, 0);
    CHECK(f.store.find(id) != nullptr);
}

TEST_CASE("CommandBlockStore.aFloodOfCyclesStaysWithinTheCap", "[semanticblocks][blockstore]")
{
    // A `cat` of a recorded session replays OSC 133 as fast as the pty delivers it.
    auto f = StoreFixture { CommandBlockStoreLimits { .maxRecords = 1000 } };
    auto largest = size_t { 0 };
    auto lastId = CommandBlockId {};
    for (auto const i: std::views::iota(0, 100'000))
    {
        lastId = f.runCycle(i);
        largest = std::max(largest, f.store.size());
    }

    CHECK(largest <= 1000);
    CHECK(f.store.size() == 1000);
    REQUIRE(f.store.lastFinished() != nullptr);
    CHECK(f.store.lastFinished()->id == lastId);
    CHECK(f.store.find(CommandBlockId { 1 }) == nullptr); // the oldest went first
}

// }}}

// {{{ enrichment, head positions, adoption

namespace
{

/// An OSC 133;C reporting @p commandLine, run in the OSC 3008 context @p context of type @p type.
[[nodiscard]] CommandStart reportedIn(std::string commandLine, ContextId context, ContextType type)
{
    auto start = reported(std::move(commandLine));
    start.context = context;
    start.contextType = type;
    return start;
}

} // namespace

TEST_CASE("CommandBlockStore.tellsItsListenerOfEveryChangeToARecordItHolds", "[semanticblocks][blockstore]")
{
    // Whatever changes what a record says tells the listener which record: a start, a reported or an
    // implied end, an outcome or a command line that arrives late, a daemon's newer copy. A birth at a
    // prompt, a head that moved and a record forgotten change nothing anyone has read.
    auto f = StoreFixture {};
    auto const command = ContextId { 2 };

    auto const first = f.store.promptStarted(0, 0);
    CHECK(f.takeChanges().empty());

    f.store.commandStarted(CommandStart { .context = command, .contextType = ContextType::Command });
    CHECK(f.takeChanges() == std::vector { first });

    f.store.commandStarted(reported("make")); // a repeated ;C, filling in the command line
    CHECK(f.takeChanges() == std::vector { first });

    std::ignore = f.store.commandFinished(1);
    CHECK(f.takeChanges() == std::vector { first });

    f.store.enrichOutcome(command,
                          ContextOutcome { .exit = ContextExit::Interrupt, .signal = ContextSignal::Term });
    CHECK(f.takeChanges() == std::vector { first });

    auto const second = f.store.promptStarted(1, 0);
    f.store.commandStarted(CommandStart {});
    CHECK(f.takeChanges() == std::vector { second });
    std::ignore = f.store.promptStarted(2, 0); // supersedes the running command: an implied end
    CHECK(f.takeChanges() == std::vector { second });

    f.store.updateHeadPosition(first, 5, 1);
    std::ignore = f.store.placeHead(second, 6, 2);
    f.store.retire(std::array { first });
    CHECK(f.takeChanges().empty());

    auto newer = *f.store.find(second);
    newer.exitCode = 3;
    std::ignore = f.store.adopt(std::move(newer), AdoptMode::Live);
    CHECK(f.takeChanges() == std::vector { second });
}

TEST_CASE("CommandBlockStore.tellsItsListenerTheNewestCommandWithEachChange", "[semanticblocks][blockstore]")
{
    // What a finish notice the next command withdraws compares its block with, carried with the change so the
    // listener reads nothing of the store mid-transition.
    auto f = StoreFixture {};
    auto const told = [](CommandBlockId id, CommandBlockId lastCommand) {
        return CommandBlockChange { .id = id, .lastCommand = lastCommand };
    };
    auto const command = ContextId { 2 };

    // Nothing ran yet: a change to the prompt's record names no command.
    auto const first = f.store.promptStarted(0, 0);
    f.store.foreignDirectoryReported();
    CHECK(f.changes == std::vector { told(first, CommandBlockId {}) });
    f.changes.clear();

    f.store.commandStarted(CommandStart { .context = command, .contextType = ContextType::Command });
    std::ignore = f.store.commandFinished(0);
    CHECK(f.changes == std::vector { told(first, first), told(first, first) });
    f.changes.clear();

    // A newer prompt runs nothing yet, so the block before stays the newest command -- also for a change
    // that reaches that block late.
    auto const second = f.store.promptStarted(1, 0);
    f.store.enrichOutcome(command, ContextOutcome { .exit = ContextExit::Failure });
    CHECK(f.changes == std::vector { told(first, first) });
    f.changes.clear();

    // Once it runs one, it is; and so it stays when a newer prompt ends it.
    f.store.commandStarted(CommandStart {});
    std::ignore = f.store.promptStarted(2, 0);
    CHECK(f.changes == std::vector { told(second, second), told(second, second) });
}

TEST_CASE("CommandBlockStore.aHeadLeavingTheGridIsStampedWithoutTellingTheListener",
          "[semanticblocks][blockstore]")
{
    // A replica must hear that a head row is gone, so the record is stamped. But what the record says is
    // unchanged, and the store learns it inside a reader's look at it -- on whichever thread reads -- so the
    // listener, which hears only of transitions, is not told.
    auto f = StoreFixture {};
    auto const first = f.runCycle(0);
    auto const second = f.runCycle(1);
    std::ignore = f.takeChanges();
    auto const before = f.store.contentRevision();

    f.store.headsLeft(0, 0, 1, {});                    // the history let go of the first head
    f.store.headsLeft(0, 1, 1, std::array { second }); // an erase left nothing of the second block

    CHECK(f.takeChanges().empty());
    CHECK(f.store.contentRevision() == before + 2);
    CHECK(f.store.stampedRecords()[0].changeStamp == before + 1);
    CHECK(f.store.stampedRecords()[1].changeStamp == before + 2);
    CHECK(f.store.find(first) != nullptr);
}

TEST_CASE("CommandBlockStore.enrichOutcomeRecordsHowACommandDiedWithoutClosingIt",
          "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const killed = ContextOutcome { .exit = ContextExit::Crash, .signal = ContextSignal::Segv };
    auto const shell = ContextId { 1 };
    auto const command = ContextId { 2 };

    SECTION("before ;D: the running block keeps it through the finish")
    {
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reportedIn("crash", command, ContextType::Command));
        auto const before = f.store.revision();
        f.store.enrichOutcome(command, killed);
        CHECK(f.store.revision() == before + 1);
        REQUIRE(f.store.current() != nullptr);
        CHECK(f.store.current()->state == CommandBlockState::Running); // enriched, not closed

        auto const summary = f.store.commandFinished(139);
        REQUIRE(summary.has_value());
        CHECK(summary->outcome == killed);
        CHECK(summary->exitCode == 139);
    }

    SECTION("after ;D, even after the next prompt: the finished block gains it")
    {
        // A shell emitting ;D and ;A from one hook ahead of its OSC 3008 one sends the end= after both.
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reportedIn("crash", command, ContextType::Command));
        std::ignore = f.store.commandFinished(139);
        f.store.promptStarted(1, 0);
        auto const before = f.store.revision();
        f.store.enrichOutcome(command, killed);
        CHECK(f.store.revision() == before + 1);

        auto const* finished = f.store.lastFinished();
        REQUIRE(finished != nullptr);
        CHECK(finished->outcome == killed);
        CHECK(finished->exitCode == 139);
        CHECK(finished->end == CommandBlockEnd::Reported);
    }

    SECTION("a command context started beneath the ;C's while the command ran is the command's own")
    {
        // The shell ran its OSC 133 hook first: the ;C saw only the shell's context.
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reportedIn("crash", shell, ContextType::Shell));
        f.store.commandContextStarted(command, shell);
        REQUIRE(f.store.current() != nullptr);
        CHECK(f.store.current()->commandContext == command);

        f.store.enrichOutcome(command, killed);
        CHECK(f.store.current()->outcome == killed);
    }

    SECTION("an end= that is not the command's own changes nothing")
    {
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reportedIn("ssh", shell, ContextType::Shell));
        f.store.commandContextStarted(command, shell);
        f.store.commandContextStarted(ContextId { 3 }, command); // nested inside: not the command's own
        auto const before = f.store.revision();
        f.store.enrichOutcome(ContextId { 3 }, killed);
        f.store.enrichOutcome(shell, killed);
        CHECK(f.store.revision() == before);
        REQUIRE(f.store.current() != nullptr);
        CHECK(f.store.current()->commandContext == command);
        CHECK(f.store.current()->outcome == ContextOutcome {});
    }

    SECTION("only the first command context started beneath the ;C's is the command's own")
    {
        // A sibling starting later beneath the same shell, the command still running, does not take over.
        auto const sibling = ContextId { 5 };
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reportedIn("crash", shell, ContextType::Shell));
        f.store.commandContextStarted(command, shell);
        f.store.commandContextStarted(sibling, shell);
        REQUIRE(f.store.current() != nullptr);
        CHECK(f.store.current()->commandContext == command);

        auto const before = f.store.revision();
        f.store.enrichOutcome(sibling, killed);
        CHECK(f.store.revision() == before);
        CHECK(f.store.current()->outcome == ContextOutcome {});
        f.store.enrichOutcome(command, killed);
        CHECK(f.store.current()->outcome == killed);
    }

    SECTION("a command context started after the command finished is not its own")
    {
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reportedIn("true", shell, ContextType::Shell));
        std::ignore = f.store.commandFinished(0);
        f.store.promptStarted(1, 0);
        auto const before = f.store.revision();
        f.store.commandContextStarted(command, shell);
        f.store.enrichOutcome(command, killed);
        CHECK(f.store.revision() == before);
        REQUIRE(f.store.lastFinished() != nullptr);
        CHECK(f.store.lastFinished()->outcome == ContextOutcome {});
    }

    SECTION("once the next command started, an earlier one gains nothing")
    {
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reportedIn("first", command, ContextType::Command));
        std::ignore = f.store.commandFinished(0);
        f.store.promptStarted(1, 0);
        f.store.commandStarted(reportedIn("second", ContextId { 4 }, ContextType::Command));
        auto const before = f.store.revision();
        f.store.enrichOutcome(command, killed);
        CHECK(f.store.revision() == before);
        REQUIRE(f.store.lastFinished() != nullptr);
        CHECK(f.store.lastFinished()->outcome == ContextOutcome {});
    }

    SECTION("an outcome already recorded is kept")
    {
        f.store.promptStarted(0, 0);
        f.store.commandStarted(reportedIn("crash", command, ContextType::Command));
        std::ignore = f.store.commandFinished(130, ContextOutcome { .exit = ContextExit::Interrupt });
        f.store.enrichOutcome(command, killed);
        REQUIRE(f.store.lastFinished() != nullptr);
        CHECK(f.store.lastFinished()->outcome.exit == ContextExit::Interrupt);
    }

    SECTION("nothing to enrich changes nothing")
    {
        auto const empty = f.store.revision();
        f.store.enrichOutcome(command, killed); // an empty store
        CHECK(f.store.revision() == empty);
        f.store.promptStarted(0, 0);
        auto const prompting = f.store.revision();
        f.store.enrichOutcome(command, killed);            // a prompt has no command that could have died
        f.store.commandContextStarted(command, shell);     // nor one whose context could start
        f.store.enrichOutcome(command, ContextOutcome {}); // and an end= that says nothing adds nothing
        CHECK(f.store.revision() == prompting);
    }
}

TEST_CASE("CommandBlockStore.updateHeadPositionMovesOnlyWhatChanged", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const id = f.store.promptStarted(5, 1);
    auto const before = f.store.revision();

    f.store.updateHeadPosition(id, 5, 1);
    CHECK(f.store.revision() == before);

    f.store.updateHeadPosition(id, 12, 2);
    CHECK(f.store.revision() == before + 1);
    REQUIRE(f.store.find(id) != nullptr);
    CHECK(f.store.find(id)->headStableId == 12);
    CHECK(f.store.find(id)->headIdGeneration == 2);

    // Either half alone is a move: a reflow can hand the same stable-id number out in a new generation.
    f.store.updateHeadPosition(id, 12, 3);
    CHECK(f.store.revision() == before + 2);
    CHECK(f.store.find(id)->headIdGeneration == 3);
    f.store.updateHeadPosition(id, 13, 3);
    CHECK(f.store.revision() == before + 3);
    CHECK(f.store.find(id)->headStableId == 13);

    f.store.updateHeadPosition(CommandBlockId { 99 }, 1, 1);
    CHECK(f.store.revision() == before + 3);
}

TEST_CASE("CommandBlockStore.updateHeadPositionIsSafeFromInsideTheVisitor", "[semanticblocks][blockstore]")
{
    // A re-sync may re-head every record this way, from inside the walk over them.
    auto f = StoreFixture {};
    for (auto const i: std::views::iota(0, 3))
        f.runCycle(i);

    f.store.forEachRecord([&](CommandBlockRecord const& record) {
        f.store.updateHeadPosition(record.id, record.headStableId + 100, 1);
    });

    auto heads = std::vector<int64_t> {};
    f.store.forEachRecord([&](CommandBlockRecord const& record) {
        CHECK(record.headIdGeneration == 1);
        heads.push_back(record.headStableId);
    });
    CHECK(heads == std::vector<int64_t> { 100, 101, 102 });
    CHECK(recordIds(f.store) == std::vector<uint32_t> { 1, 2, 3 });
}

TEST_CASE("CommandBlockStore.placeHeadKeepsTheFirstRowOfferedInAGeneration", "[semanticblocks][blockstore]")
{
    // A re-sync offers a block's rows lowest first, and the lowest is the head: a later offer in the same
    // generation is not a move.
    auto f = StoreFixture {};
    auto const id = f.store.promptStarted(5, 1);
    auto const before = f.store.revision();

    CHECK(f.store.placeHead(id, 12, 2) == HeadPlacement::Placed);
    CHECK(f.store.revision() == before + 1);
    REQUIRE(f.store.find(id) != nullptr);
    CHECK(f.store.find(id)->headStableId == 12);
    CHECK(f.store.find(id)->headIdGeneration == 2);

    CHECK(f.store.placeHead(id, 7, 2) == HeadPlacement::Unchanged);
    CHECK(f.store.find(id)->headStableId == 12);
    CHECK(f.store.revision() == before + 1);

    CHECK(f.store.placeHead(CommandBlockId { 99 }, 1, 3) == HeadPlacement::Unchanged);
    CHECK(f.store.revision() == before + 1);
}

TEST_CASE("CommandBlockStore.retireUnplacedHeadsEvictsOnlyWhatNoRowPlaced", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const placed = f.store.promptStarted(5, 1);
    f.store.commandStarted(CommandStart {}); // so the next prompt keeps it
    auto const lost = f.store.promptStarted(6, 1);
    REQUIRE(f.store.placeHead(placed, 40, 2) == HeadPlacement::Placed);
    auto const before = f.store.revision();

    f.store.retireUnplacedHeads(2, -1);
    CHECK(f.store.revision() != before);
    REQUIRE(f.store.find(lost) != nullptr);
    CHECK(f.store.find(lost)->headStableId == -1);
    CHECK(f.store.find(lost)->headIdGeneration == 2);
    CHECK(f.store.find(placed)->headStableId == 40);

    // Nothing left unplaced: nothing changes, so the revision stays.
    auto const after = f.store.revision();
    f.store.retireUnplacedHeads(2, -1);
    CHECK(f.store.revision() == after);
}

namespace
{

/// A token source handing out {1, 2, 3, n}, n counting up from 1: a known token, and a fresh one per session.
[[nodiscard]] SemanticBlockTokenSource countingTokens()
{
    return [next = uint16_t { 0 }]() mutable {
        ++next;
        return SemanticBlockToken { 1, 2, 3, next };
    };
}

} // namespace

TEST_CASE("SemanticBlockTracker.eachSessionTakesItsTokenFromTheInjectedSource",
          "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto tracker = SemanticBlockTracker { countingTokens() };
    CHECK_FALSE(tracker.token().has_value()); // nothing is drawn before a session starts

    tracker.enable(f.store);
    CHECK(tracker.token() == SemanticBlockToken { 1, 2, 3, 1 });
    CHECK(tracker.validateToken(SemanticBlockToken { 1, 2, 3, 1 }));

    tracker.disable();
    tracker.enable(f.store);
    CHECK(tracker.token() == SemanticBlockToken { 1, 2, 3, 2 }); // a fresh one per session
    CHECK_FALSE(tracker.validateToken(SemanticBlockToken { 1, 2, 3, 1 }));
}

TEST_CASE("SemanticBlockTracker.randomTokensAreNotRepeated", "[semanticblocks][blockstore]")
{
    // What production passes. Two draws agree once in 2^64.
    auto const tokens = randomSemanticBlockTokens();
    CHECK(tokens() != tokens());
}

TEST_CASE("SemanticBlockTracker.finishedBlocksAreTheNewestVisibleReportedOnes",
          "[semanticblocks][blockstore]")
{
    // Read newest first: the walk stops at the count, or at the first block from before the reader's session.
    auto f = StoreFixture {};
    auto const run = [&f] {
        auto const id = f.store.promptStarted(0, 0);
        f.store.commandStarted(CommandStart {});
        return id;
    };

    std::ignore = run(); // finished before the session starts: hidden
    std::ignore = f.store.commandFinished(0);
    auto tracker = SemanticBlockTracker { countingTokens() };
    tracker.enable(f.store);

    auto const older = run();
    std::ignore = f.store.commandFinished(0);
    run(); // closed by the next prompt: finished, but with no end reported
    auto const newer = run();
    std::ignore = f.store.commandFinished(1);
    f.store.promptStarted(0, 0); // still at its prompt

    auto const finished = [&](size_t count) {
        auto ids = std::vector<CommandBlockId> {};
        for (auto const* record: tracker.finishedBlocks(f.store, count))
            ids.push_back(record->id);
        return ids;
    };
    CHECK(finished(5) == std::vector { newer, older });
    CHECK(finished(1) == std::vector { newer });
    CHECK(finished(0).empty());
}

TEST_CASE("SemanticBlockTracker.aSessionSeesTheBlocksWhoseCommandIsYetToStart",
          "[semanticblocks][blockstore]")
{
    // A shell enables the mode from its prompt hook, after the prompt's ;A: the command typed at that prompt
    // starts after the session did, so its block is the reader's to see. A command already running when the
    // session starts -- typically the reader itself -- is not.
    auto f = StoreFixture {};
    std::ignore = f.runCycle(0);
    auto const prompting = f.store.promptStarted(0, 0);
    auto tracker = SemanticBlockTracker { countingTokens() };

    SECTION("the block at its prompt")
    {
        tracker.enable(f.store);
        CHECK(tracker.inProgressBlock(f.store) == f.store.find(prompting));

        f.store.commandStarted(CommandStart {});
        std::ignore = f.store.commandFinished(0);
        auto const finished = tracker.finishedBlocks(f.store, 5);
        REQUIRE(finished.size() == 1);
        CHECK(finished.front()->id == prompting);
    }

    SECTION("the block whose command runs")
    {
        f.store.commandStarted(CommandStart {});
        tracker.enable(f.store);
        CHECK(tracker.inProgressBlock(f.store) == nullptr);

        std::ignore = f.store.commandFinished(0);
        CHECK(tracker.finishedBlocks(f.store, 5).empty());
    }
}

TEST_CASE("SemanticBlockTracker.aSessionSeesTheBlocksMintedAfterTheIdsStartOver",
          "[semanticblocks][blockstore]")
{
    // The floor is an id, and an id names a block only within its epoch: once the ids start over at 1, every
    // block minted is numbered below the floor, though every one of them started after the session did.
    constexpr auto LargestId = std::numeric_limits<CommandBlockId::inner_type>::max();
    auto f = StoreFixture {};
    auto record = mirroredFinish(7, 0);
    record.id = CommandBlockId { LargestId - 1 };
    std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
    auto tracker = SemanticBlockTracker { countingTokens() };
    tracker.enable(f.store);

    auto const afterTheWrap = f.runCycle(0);
    REQUIRE(afterTheWrap == CommandBlockId { 1 });
    auto const finished = tracker.finishedBlocks(f.store, 5);
    REQUIRE(finished.size() == 1);
    CHECK(finished.front()->id == afterTheWrap);

    auto const prompting = f.store.promptStarted(0, 0);
    CHECK(tracker.inProgressBlock(f.store) == f.store.find(prompting));
}

TEST_CASE("CommandBlockStore.adoptTakesMirroredRecords", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    auto const running = mirroredRecord(7, CommandBlockState::Running);

    SECTION("a snapshot brings history in silently")
    {
        auto const finished = mirroredFinish(7, 0);
        CHECK_FALSE(f.store.adopt(finished, AdoptMode::Snapshot).has_value());
        REQUIRE(f.store.find(CommandBlockId { 7 }) != nullptr);
        CHECK(*f.store.find(CommandBlockId { 7 }) == finished);
    }

    SECTION("a live delta announces the transition to Finished, once")
    {
        CHECK_FALSE(f.store.adopt(running, AdoptMode::Live).has_value());

        auto finished = mirroredFinish(7, 2);
        finished.duration = 3s;
        auto const summary = f.store.adopt(finished, AdoptMode::Live);
        REQUIRE(summary.has_value());
        CHECK(summary->id == CommandBlockId { 7 });
        CHECK(summary->exitCode == 2);
        CHECK(summary->duration == 3s);

        // The same delta again -- a resend, a reattach -- announces nothing.
        CHECK_FALSE(f.store.adopt(finished, AdoptMode::Live).has_value());
    }

    SECTION("a finished block that changes afterwards is not announced again")
    {
        // The host enriches a block after its ;D -- an OSC 3008 end= naming the signal -- and ships the
        // change as a live delta. Its finish was announced already, or arrived as history; either way, once.
        auto const finished = mirroredFinish(7, 139);

        SECTION("finished by a live delta")
        {
            CHECK_FALSE(f.store.adopt(running, AdoptMode::Live).has_value());
            CHECK(f.store.adopt(finished, AdoptMode::Live).has_value());
        }
        SECTION("brought in by an attach snapshot")
        {
            CHECK_FALSE(f.store.adopt(finished, AdoptMode::Snapshot).has_value());
        }

        auto const revision = f.store.revision();
        auto enriched = finished;
        enriched.outcome = ContextOutcome { .exit = ContextExit::Crash, .signal = ContextSignal::Segv };
        CHECK_FALSE(f.store.adopt(enriched, AdoptMode::Live).has_value());
        CHECK(f.store.revision() == revision + 1);
        REQUIRE(f.store.find(CommandBlockId { 7 }) != nullptr);
        CHECK(f.store.find(CommandBlockId { 7 })->outcome == enriched.outcome);
    }

    SECTION("an implicit close is never announced")
    {
        auto superseded = running;
        superseded.state = CommandBlockState::Finished;
        superseded.end = CommandBlockEnd::Implicit;
        CHECK_FALSE(f.store.adopt(superseded, AdoptMode::Live).has_value());
    }

    SECTION("a record arriving out of order is still found, and its id is never minted again")
    {
        auto newer = running;
        newer.id = CommandBlockId { 9 };
        std::ignore = f.store.adopt(newer, AdoptMode::Snapshot);
        std::ignore = f.store.adopt(running, AdoptMode::Snapshot); // id 7, older

        CHECK(recordIds(f.store) == std::vector<uint32_t> { 7, 9 });
        CHECK(f.store.find(CommandBlockId { 7 }) != nullptr);
        CHECK(f.store.promptStarted(0, 0) == CommandBlockId { 10 });
    }

    SECTION("an id of zero is refused")
    {
        auto nameless = running;
        nameless.id = CommandBlockId {};
        CHECK_FALSE(f.store.adopt(nameless, AdoptMode::Live).has_value());
        CHECK(f.store.size() == 0);
    }

    SECTION("a mirrored command line is held to the same bound")
    {
        auto huge = running;
        huge.commandLine = vtbackend::CommandLineText { std::string(MaxRecordedCommandLineBytes * 2, 'x') };
        std::ignore = f.store.adopt(huge, AdoptMode::Snapshot);
        REQUIRE(f.store.find(CommandBlockId { 7 }) != nullptr);
        CHECK(f.store.find(CommandBlockId { 7 })->commandLine.raw().size() <= MaxRecordedCommandLineBytes);
    }

    SECTION("a mirrored working directory is held to the same bound: dropped, never cut")
    {
        auto const adoptedIn = [&](size_t pathBytes) {
            auto record = running;
            record.workingDirectory = { .path = "/" + std::string(pathBytes - 1, 'd'),
                                        .locality = ContextLocality::Foreign };
            std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
            REQUIRE(f.store.find(CommandBlockId { 7 }) != nullptr);
            return f.store.find(CommandBlockId { 7 })->workingDirectory;
        };

        CHECK(adoptedIn(MaxRecordedWorkingDirectoryBytes).path.size() == MaxRecordedWorkingDirectoryBytes);
        CHECK(adoptedIn(MaxRecordedWorkingDirectoryBytes + 1) == WorkingDirectorySnapshot {});
    }
}

TEST_CASE("CommandBlockStore.adoptHoldsARecordsTimesToTheRangeATerminalComputesWith",
          "[semanticblocks][blockstore]")
{
    // Whoever produced the record -- a daemon, a tmux server, a restore -- its duration added to its start
    // is when it ended, and either instant is subtracted from a clock. Near the limits of a nanosecond
    // clock each overflows unless the store holds them as it takes the record.
    using std::chrono::steady_clock;
    using std::chrono::system_clock;
    auto f = StoreFixture {};

    SECTION("past either edge, at the edge it overshot")
    {
        auto record = mirroredFinish(7, 0);
        record.promptStartedAt = system_clock::time_point::min();
        record.commandStartedAt = system_clock::time_point::max();
        record.duration = steady_clock::duration::max();
        std::ignore = f.store.adopt(record, AdoptMode::Snapshot);

        auto const* const held = f.store.find(CommandBlockId { 7 });
        REQUIRE(held != nullptr);
        CHECK(held->promptStartedAt == EarliestHeldInstant);
        CHECK(held->commandStartedAt == LatestHeldInstant);
        CHECK(held->duration == LongestHeldSpan);
    }

    SECTION("a negative length is no length")
    {
        auto record = mirroredFinish(7, 0);
        record.duration = -1s;
        std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
        REQUIRE(f.store.find(CommandBlockId { 7 }) != nullptr);
        CHECK(f.store.find(CommandBlockId { 7 })->duration == steady_clock::duration::zero());
    }

    SECTION("inside the range, as taken")
    {
        auto record = mirroredFinish(7, 0);
        record.promptStartedAt = system_clock::time_point { 1'700'000'000s };
        record.commandStartedAt = LatestHeldInstant;
        record.duration = LongestHeldSpan;
        std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
        REQUIRE(f.store.find(CommandBlockId { 7 }) != nullptr);
        CHECK(*f.store.find(CommandBlockId { 7 }) == record);
    }
}

TEST_CASE("CommandBlockStore.adoptMovesTheRevisionOnlyWhenSomethingChanged", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};

    auto record = mirroredRecord(7, CommandBlockState::Running);
    std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
    CHECK(f.revisionMoved());
    std::ignore = f.store.adopt(record, AdoptMode::Snapshot); // a resend
    CHECK_FALSE(f.revisionMoved());
    std::ignore = f.store.adopt(record, AdoptMode::Live); // the same record by the other way in
    CHECK_FALSE(f.revisionMoved());

    record = mirroredFinish(7, 0);
    std::ignore = f.store.adopt(record, AdoptMode::Live);
    CHECK(f.revisionMoved());

    // The bounds apply before the comparison, so resending an over-long record is a resend too.
    record.commandLine = vtbackend::CommandLineText { std::string(MaxRecordedCommandLineBytes * 2, 'x') };
    record.workingDirectory = { .path = std::string(MaxRecordedWorkingDirectoryBytes + 1, 'd') };
    std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
    CHECK(f.revisionMoved());
    std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
    CHECK_FALSE(f.revisionMoved());
}

TEST_CASE("CommandBlockStore.adoptingARecordOlderThanAllAFullStoreKeepsChangesNothing",
          "[semanticblocks][blockstore]")
{
    // Every record a full store keeps is newer, so this one would be evicted the moment it arrived.
    auto f = StoreFixture { CommandBlockStoreLimits { .maxRecords = 2 } };
    auto record = mirroredRecord(7, CommandBlockState::Running);
    record.state = CommandBlockState::Finished;
    for (auto const id: { 5U, 6U })
    {
        record.id = CommandBlockId { id };
        std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
    }
    auto const revision = f.store.revision();

    record.id = CommandBlockId { 3 };
    CHECK_FALSE(f.store.adopt(record, AdoptMode::Live).has_value());
    CHECK(f.store.find(CommandBlockId { 3 }) == nullptr);
    CHECK(recordIds(f.store) == std::vector<uint32_t> { 5, 6 });
    CHECK(f.store.revision() == revision);
}

TEST_CASE("CommandBlockStore.adoptingTheTopmostIdsMakesTheNextPromptStartOver",
          "[semanticblocks][blockstore]")
{
    // A store never mints the largest id: mint() starts over at one when it reaches it. Adopting the id just
    // below it, or -- from a host that broke that rule -- the largest itself, leaves the counter at the wrap,
    // so ids keep ascending through the next prompt.
    constexpr auto LargestId = std::numeric_limits<CommandBlockId::inner_type>::max();
    auto f = StoreFixture {};
    auto record = mirroredRecord(7, CommandBlockState::Running);

    SECTION("the id below the wrap")
    {
        record.id = CommandBlockId { LargestId - 1 };
    }
    SECTION("the largest id")
    {
        record.id = CommandBlockId { LargestId };
    }

    std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
    REQUIRE(f.store.find(record.id) != nullptr);

    CHECK(f.store.promptStarted(0, 0) == CommandBlockId { 1 });
    CHECK(f.store.size() == 1);
    CHECK(recordIds(f.store) == std::vector<uint32_t> { 1 });
}

TEST_CASE("CommandBlockStore.theIdEpochMovesExactlyWhenTheIdsStartOver", "[semanticblocks][blockstore]")
{
    // An id names a block only within its epoch, so whoever holds ids -- the rows stamped with them, a
    // reader's visibility floor -- learns from the epoch that they now name nothing.
    constexpr auto LargestId = std::numeric_limits<CommandBlockId::inner_type>::max();
    auto f = StoreFixture {};
    auto const start = f.store.idEpoch();

    std::ignore = f.runCycle(0);
    CHECK(f.store.idEpoch() == start); // an ordinary prompt

    auto record = mirroredRecord(7, CommandBlockState::Running);
    record.id = CommandBlockId { LargestId - 1 };
    std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
    CHECK(f.store.idEpoch() == start); // adopting mints nothing

    REQUIRE(f.store.promptStarted(0, 0) == CommandBlockId { 1 });
    CHECK(f.store.idEpoch() == start + 1); // the wrap

    f.store.clear();
    CHECK(f.store.promptStarted(0, 0) == CommandBlockId { 2 });
    CHECK(f.store.idEpoch() == start + 1); // forgetting the records does not reuse their ids
}

// }}}

// {{{ change stamps

namespace
{

/// Every record's id and change stamp, oldest first.
[[nodiscard]] std::vector<std::pair<uint32_t, uint64_t>> stampsOf(CommandBlockStore const& store)
{
    auto stamps = std::vector<std::pair<uint32_t, uint64_t>> {};
    for (auto const entry: store.stampedRecords())
        stamps.emplace_back(unbox(entry.record.id), entry.changeStamp);
    return stamps;
}

/// @return @p id's change stamp in @p store, or zero -- which no record carries -- when it holds no such id.
[[nodiscard]] uint64_t stampOf(CommandBlockStore const& store, CommandBlockId id)
{
    for (auto const entry: store.stampedRecords())
        if (entry.record.id == id)
            return entry.changeStamp;
    return 0;
}

} // namespace

TEST_CASE("CommandBlockStore.everyChangeThatTravelsStampsItsRecord", "[semanticblocks][blockstore]")
{
    // A replica holds every field but the head position and the command's own context, so exactly the
    // changes to those fields -- and records coming and going -- move the content revision; the record
    // changed carries the new value.
    auto f = StoreFixture {};
    auto seen = f.store.contentRevision();
    auto const moved = [&] {
        auto const now = f.store.contentRevision();
        return std::exchange(seen, now) != now;
    };
    auto const shell = ContextId { 1 };
    auto const command = ContextId { 2 };

    auto const first = f.store.promptStarted(0, 0);
    CHECK(moved());
    CHECK(stampOf(f.store, first) == f.store.contentRevision());

    f.store.commandStarted(reportedIn("crash", shell, ContextType::Shell));
    CHECK(moved());
    CHECK(stampOf(f.store, first) == f.store.contentRevision());

    f.store.commandContextStarted(command, shell); // the command's own context stays behind
    REQUIRE(f.store.current() != nullptr);
    REQUIRE(f.store.current()->commandContext == command);
    CHECK_FALSE(moved());

    std::ignore = f.store.commandFinished(139);
    CHECK(moved());
    CHECK(stampOf(f.store, first) == f.store.contentRevision());

    auto const second = f.store.promptStarted(1, 0);
    CHECK(moved());
    auto const finishedAt = stampOf(f.store, first);
    CHECK(stampOf(f.store, second) == f.store.contentRevision());

    // An enrichment after the next prompt restamps the finished record, not the newest.
    f.store.enrichOutcome(command,
                          ContextOutcome { .exit = ContextExit::Crash, .signal = ContextSignal::Segv });
    CHECK(moved());
    CHECK(stampOf(f.store, first) == f.store.contentRevision());
    CHECK(stampOf(f.store, first) > finishedAt);

    // A prompt that ran nothing is discarded by the next: a record a replica holds is gone.
    std::ignore = f.store.promptStarted(2, 0);
    CHECK(moved());
    CHECK(stampOf(f.store, second) == 0);

    f.store.clear();
    CHECK(moved());
    CHECK(f.store.stampedRecords().empty());
    f.store.clear(); // nothing left to drop
    CHECK_FALSE(moved());
}

TEST_CASE("CommandBlockStore.headPlacementLeavesEveryChangeStampAlone", "[semanticblocks][blockstore]")
{
    // A reflow, an erase or a scroll inside margins re-heads blocks: the gutter's caches must see it, a
    // replica -- which heads its own -- must not.
    auto f = StoreFixture {};
    auto const older = f.runCycle(0);
    auto const newer = f.store.promptStarted(1, 0);
    auto const content = f.store.contentRevision();
    auto const stamps = stampsOf(f.store);
    auto const revision = f.store.revision();

    f.store.updateHeadPosition(older, 40, 1);
    CHECK(f.store.placeHead(newer, 41, 2) == HeadPlacement::Placed);
    f.store.retireUnplacedHeads(2, -1);

    CHECK(f.store.revision() != revision);
    CHECK(f.store.contentRevision() == content);
    CHECK(stampsOf(f.store) == stamps);
}

TEST_CASE("CommandBlockStore.stampsStayWithTheirRecordsThroughInsertsAndEvictions",
          "[semanticblocks][blockstore]")
{
    // The stamps sit beside the records, so every insert and eviction has to move both.
    auto f = StoreFixture { CommandBlockStoreLimits { .maxRecords = 3 } };
    auto record = mirroredRecord(7, CommandBlockState::Running);
    auto const adopt = [&](uint32_t id) {
        record.id = CommandBlockId { id };
        std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
        return f.store.contentRevision();
    };

    auto const two = adopt(2);
    auto const six = adopt(6);
    auto const four = adopt(4); // between the two
    CHECK(stampsOf(f.store)
          == std::vector<std::pair<uint32_t, uint64_t>> { { 2, two }, { 4, four }, { 6, six } });

    auto const eight = adopt(8); // evicts the oldest
    CHECK(stampsOf(f.store)
          == std::vector<std::pair<uint32_t, uint64_t>> { { 4, four }, { 6, six }, { 8, eight } });

    record.id = CommandBlockId { 6 }; // a resend changes nothing, so stamps nothing
    std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
    CHECK(f.store.contentRevision() == eight);

    // The ids starting over drop every record, stamps included.
    record.id = CommandBlockId { std::numeric_limits<CommandBlockId::inner_type>::max() - 1 };
    std::ignore = f.store.adopt(record, AdoptMode::Snapshot);
    auto const wrapped = f.store.promptStarted(0, 0);
    CHECK(stampsOf(f.store)
          == std::vector<std::pair<uint32_t, uint64_t>> { { unbox(wrapped), f.store.contentRevision() } });
}

namespace
{

/// Every record's id and head, oldest first.
[[nodiscard]] std::vector<std::tuple<uint32_t, int64_t, uint64_t>> headsOf(CommandBlockStore const& store)
{
    auto heads = std::vector<std::tuple<uint32_t, int64_t, uint64_t>> {};
    store.forEachRecord([&](CommandBlockRecord const& record) {
        heads.emplace_back(unbox(record.id), record.headStableId, record.headIdGeneration);
    });
    return heads;
}

/// @p from without the entries whose id is one of @p ids.
template <typename Entry>
[[nodiscard]] std::vector<Entry> without(std::vector<Entry> from, std::vector<uint32_t> const& ids)
{
    std::erase_if(
        from, [&](Entry const& entry) { return std::ranges::find(ids, std::get<0>(entry)) != ids.end(); });
    return from;
}

} // namespace

TEST_CASE("CommandBlockStore.retireForgetsTheNamedRecordsAndTouchesNoOther", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    for (auto const head: std::views::iota(int64_t { 0 }, int64_t { 5 }))
        std::ignore = f.runCycle(head * 10); // ids 1..5, headed at rows 0, 10, .. 40

    auto const retired = GENERATE(std::vector<uint32_t> { 1 },        // the oldest
                                  std::vector<uint32_t> { 5 },        // the newest
                                  std::vector<uint32_t> { 3 },        // one in the middle
                                  std::vector<uint32_t> { 4, 1, 5 }); // several, in any order
    INFO("retiring " << retired.size() << " record(s), the first " << retired.front());

    auto const stamps = stampsOf(f.store);
    auto const heads = headsOf(f.store);
    auto const content = f.store.contentRevision();
    auto const revision = f.store.revision();
    auto const ids = std::ranges::to<std::vector>(
        retired | std::views::transform([](uint32_t id) { return CommandBlockId { id }; }));

    f.store.retire(ids);

    for (auto const id: ids)
        CHECK(f.store.find(id) == nullptr);
    CHECK(stampsOf(f.store) == without(stamps, retired));
    CHECK(headsOf(f.store) == without(heads, retired));
    CHECK(f.store.contentRevision() > content);
    CHECK(f.store.revision() > revision);
    // The ids are not reused.
    CHECK(f.store.promptStarted(50, 0) == CommandBlockId { 6 });
}

TEST_CASE("CommandBlockStore.retireOfIdsItDoesNotHoldChangesNothing", "[semanticblocks][blockstore]")
{
    auto f = StoreFixture {};
    std::ignore = f.runCycle(0);
    std::ignore = f.runCycle(1);
    auto const stamps = stampsOf(f.store);
    auto const content = f.store.contentRevision();
    std::ignore = f.revisionMoved();

    f.store.retire(std::array { CommandBlockId {}, CommandBlockId { 9 } });
    f.store.retire({});

    CHECK(stampsOf(f.store) == stamps);
    CHECK(f.store.contentRevision() == content);
    CHECK_FALSE(f.revisionMoved());
}

TEST_CASE("CommandBlockStore.retiringTheRunningRecordForgetsItsRun", "[semanticblocks][blockstore]")
{
    // The running command waited for its own OSC 3008 context beneath the shell's. Once its record is
    // gone, the next command -- which brings a context of its own -- must not take a stray one for it.
    auto f = StoreFixture {};
    auto const shell = ContextId { 1 };
    auto const own = ContextId { 5 };
    std::ignore = f.store.promptStarted(0, 0);
    f.store.commandStarted(reportedIn("sleep 1", shell, ContextType::Shell));
    f.store.retire(std::array { f.store.currentId() });
    REQUIRE(f.store.size() == 0);

    std::ignore = f.store.promptStarted(1, 0);
    f.store.commandStarted(reportedIn("make", own, ContextType::Command));
    f.store.commandContextStarted(ContextId { 9 }, shell);

    REQUIRE(f.store.current() != nullptr);
    CHECK(f.store.current()->commandContext == own);
}

// }}}

// {{{ finishDetailText

TEST_CASE("CommandBlock.finishDetailText.namesTheCodeOrTheSignal", "[semanticblocks]")
{
    struct Row
    {
        FinishDetail detail;
        std::optional<int> exitCode;
        ContextOutcome outcome;
        std::string_view expected;
    };
    auto const rows = std::array {
        Row { .detail = FinishDetail::ExitCode, .exitCode = 2, .outcome = {}, .expected = "2" },
        Row { .detail = FinishDetail::ExitCode, .exitCode = 0, .outcome = {}, .expected = "" },
        // OSC 3008 alone: the code a shell would have reported, which for a bare failure is none.
        Row { .detail = FinishDetail::ExitCode,
              .exitCode = std::nullopt,
              .outcome = ContextOutcome { .exit = ContextExit::Failure, .status = 3 },
              .expected = "3" },
        Row { .detail = FinishDetail::ExitCode,
              .exitCode = std::nullopt,
              .outcome = ContextOutcome { .exit = ContextExit::Failure },
              .expected = "" },
        Row { .detail = FinishDetail::Signal, .exitCode = 137, .outcome = {}, .expected = "SIGKILL" },
        Row { .detail = FinishDetail::Signal,
              .exitCode = std::nullopt,
              .outcome = ContextOutcome { .signal = ContextSignal::Segv },
              .expected = "SIGSEGV" },
        Row { .detail = FinishDetail::None, .exitCode = 2, .outcome = {}, .expected = "" },
    };
    for (auto const& row: rows)
    {
        INFO("expected " << row.expected);
        CHECK(finishDetailText(row.detail, row.exitCode, row.outcome) == row.expected);
    }
}

// }}}

// {{{ formatCommandDuration

TEST_CASE("CommandBlock.formatCommandDuration.readsAtTheCoarsestUsefulUnit", "[semanticblocks]")
{
    struct Row
    {
        std::chrono::steady_clock::duration duration;
        std::string_view expected;
    };
    auto const rows = std::array {
        Row { 0ms, "0ms" },        Row { 850ms, "850ms" },   Row { 999ms, "999ms" },  Row { 1s, "1s" },
        Row { 12s, "12s" },        Row { 59999ms, "59s" },   Row { 60s, "1m 00s" },   Row { 185s, "3m 05s" },
        Row { 192s, "3m 12s" },    Row { 3599s, "59m 59s" }, Row { 3600s, "1h 00m" }, Row { 3725s, "1h 02m" },
        Row { 90000s, "25h 00m" },
    };
    for (auto const& row: rows)
    {
        INFO(row.expected);
        CHECK(formatCommandDuration(row.duration) == row.expected);
    }
}

TEST_CASE("CommandBlock.formatCommandDuration.aNegativeDurationReadsAsZero", "[semanticblocks]")
{
    // A wall clock stepped backwards under a running command (the sticky header's chip) must not print a
    // negative.
    CHECK(formatCommandDuration(-5s) == "0ms");
}

// }}}
