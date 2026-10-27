// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandBlockTooltip.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/shell/CommandLineText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using contour::session::BlockTooltipHint;
using contour::session::blockTooltipHintText;
using contour::session::commandBlockHeaderLines;
using contour::session::commandBlockTooltipText;
using namespace std::chrono_literals;

namespace chrono = std::chrono;

namespace
{
/// 2026-10-09 14:03:07, local civil time.
constexpr auto Started = chrono::local_days { chrono::year { 2026 } / chrono::October / 9 } + 14h + 3min + 7s;

/// A command that finished with @p exitCode after 4.25 s, in /home/user/src.
[[nodiscard]] vtbackend::CommandBlockRecord finished(int exitCode)
{
    auto record = vtbackend::CommandBlockRecord {};
    record.state = vtbackend::CommandBlockState::Finished;
    record.exitCode = exitCode;
    record.commandStartedAt = chrono::system_clock::time_point {}; // shown as Started, which stands for it
    record.duration = 4250ms;
    record.commandLine = vtbackend::CommandLineText { "make -j8" };
    record.workingDirectory.path = "/home/user/src";
    return record;
}
} // namespace

TEST_CASE("CommandBlockTooltip.hintSaysWhatAClickDoes", "[contour][gutter]")
{
    // What a mark cannot say by itself: a box folds, a disc or a cross opens the block's menu, and a
    // right-click opens it anywhere on the block.
    CHECK(blockTooltipHintText(BlockTooltipHint::Collapse) == "Click to collapse · Right-click for actions");
    CHECK(blockTooltipHintText(BlockTooltipHint::Expand) == "Click to expand · Right-click for actions");
    CHECK(blockTooltipHintText(BlockTooltipHint::Menu) == "Click for actions");
    CHECK(blockTooltipHintText(BlockTooltipHint::RightClickOnly) == "Right-click for actions");
}

TEST_CASE("CommandBlockTooltip.saysHowWhenWhereAndWhat", "[contour][gutter]")
{
    // The duration is vtbackend::formatCommandDuration's: 4.25 s reads "4s".
    CHECK(commandBlockTooltipText(finished(2), Started)
          == "Exit code 2\nTook 4s\nStarted 2026-10-09 14:03:07\nIn /home/user/src\n$ make -j8");
}

TEST_CASE("CommandBlockTooltip.theHeadingOfTheMenuIsOneFactPerLine", "[contour][gutter][blockheader]")
{
    // Command first, then the status in the chip's words, where it ran, and when it started.
    auto const now = chrono::system_clock::time_point {};
    CHECK(commandBlockHeaderLines(finished(2), Started, now)
          == std::vector<std::string> {
              "make -j8", "Failed · exit 2 · 4s", "/home/user/src", "Started 14:03:07" });

    // What the record does not know is left out, and a prompt has no heading at all.
    auto bare = vtbackend::CommandBlockRecord {};
    bare.state = vtbackend::CommandBlockState::Finished;
    bare.exitCode = 1;
    CHECK(commandBlockHeaderLines(bare, Started, now) == std::vector<std::string> { "Failed · exit 1" });
    bare.state = vtbackend::CommandBlockState::Prompting;
    CHECK(commandBlockHeaderLines(bare, Started, now).empty());
}

TEST_CASE("CommandBlockTooltip.namesTheSignalThatEndedACommand", "[contour][gutter]")
{
    // From a shell's 128 + n...
    CHECK(commandBlockTooltipText(finished(130), Started).starts_with("Exit code 130 (SIGINT)\n"));

    // ...and from an OSC 3008 outcome that says so outright.
    auto record = finished(139);
    record.outcome.signal = vtbackend::ContextSignal::Segv;
    CHECK(commandBlockTooltipText(record, Started).starts_with("Exit code 139 (SIGSEGV)\n"));

    // A code above 128 that names no signal we know is just a code; so is one naming a signal that other
    // systems number differently (138 is SIGUSR1 on Linux, SIGBUS on macOS and the BSDs).
    CHECK(commandBlockTooltipText(finished(200), Started).starts_with("Exit code 200\n"));
    CHECK(commandBlockTooltipText(finished(138), Started).starts_with("Exit code 138\n"));
}

TEST_CASE("CommandBlockTooltip.anExplicitFailureKeepsItsCode", "[contour][gutter]")
{
    // OSC 3008 said the command failed with status 130 and named no signal: that is the program's code.
    auto record = finished(130);
    record.outcome =
        vtbackend::ContextOutcome { .exit = vtbackend::ContextExit::Failure, .signal = {}, .status = 130 };
    CHECK(commandBlockTooltipText(record, Started).starts_with("Exit code 130\n"));
}

TEST_CASE("CommandBlockTooltip.leavesOutWhatTheRecordDoesNotKnow", "[contour][gutter]")
{
    auto record = vtbackend::CommandBlockRecord {};
    record.state = vtbackend::CommandBlockState::Running;
    record.commandStartedAt = chrono::system_clock::time_point {};
    CHECK(commandBlockTooltipText(record, Started) == "Running\nStarted 2026-10-09 14:03:07");

    record.state = vtbackend::CommandBlockState::Finished;
    CHECK(commandBlockTooltipText(record, Started)
          == "Finished, exit code unknown\nStarted 2026-10-09 14:03:07");

    // A prompt's start stands in for a command's that is not known...
    record.commandStartedAt.reset();
    record.promptStartedAt = chrono::system_clock::time_point {};
    CHECK(commandBlockTooltipText(record, Started).ends_with("\nStarted 2026-10-09 14:03:07"));

    // ...and a record from another process that names neither has no start to show: not the epoch, nor
    // whatever the caller resolved in its place.
    record.promptStartedAt.reset();
    CHECK(commandBlockTooltipText(record, Started) == "Finished, exit code unknown");

    // A prompt still being typed at has run nothing, and so has nothing to say.
    record.state = vtbackend::CommandBlockState::Prompting;
    CHECK(commandBlockTooltipText(record, Started).empty());
}

TEST_CASE("CommandBlockTooltip.showsTheCommandLineSanitised", "[contour][gutter]")
{
    // Both strings are whatever arrived on the PTY: a title-setting sequence and a bidi override must not
    // reach the tooltip as they are.
    // U+202E RIGHT-TO-LEFT OVERRIDE, spliced in byte by byte rather than written into a literal, where it
    // would be a misleading bidi character itself.
    auto const rightToLeftOverride = std::string { '\xE2', '\x80', '\xAE' };
    auto record = finished(0);
    record.commandLine = vtbackend::CommandLineText { "echo \033]0;owned\007 done" };
    record.workingDirectory.path = "/tmp/" + rightToLeftOverride + "evil";

    auto const text = commandBlockTooltipText(record, Started);
    CHECK_FALSE(text.contains('\033'));
    CHECK_FALSE(text.contains('\007'));
    CHECK_FALSE(text.contains(rightToLeftOverride));
    CHECK(text.contains("\xEF\xBF\xBD")); // U+FFFD in their place
}
