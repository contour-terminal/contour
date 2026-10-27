// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandBlockTooltip.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/shell/CommandLineText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using contour::session::commandBlockHeaderLines;
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

/// The header lines of @p record, the wall clock at the epoch.
[[nodiscard]] std::vector<std::string> headerOf(vtbackend::CommandBlockRecord const& record)
{
    return commandBlockHeaderLines(record, Started, chrono::system_clock::time_point {});
}
} // namespace

TEST_CASE("CommandBlockHeader.isOneFactPerLine", "[contour][gutter][blockheader]")
{
    // Command first, then the status in the chip's words (the duration is formatCommandDuration's: 4.25 s
    // reads "4s"), where it ran, and when it started. There is no hint line.
    CHECK(headerOf(finished(2))
          == std::vector<std::string> {
              "make -j8", "Failed · exit 2 · 4s", "/home/user/src", "Started 14:03:07" });
}

TEST_CASE("CommandBlockHeader.namesTheSignalThatEndedACommand", "[contour][gutter][blockheader]")
{
    // From a shell's 128 + n...
    CHECK(headerOf(finished(130)).at(1).starts_with("Killed · SIGINT"));

    // ...and from an OSC 3008 outcome that says so outright.
    auto record = finished(139);
    record.outcome.signal = vtbackend::ContextSignal::Segv;
    CHECK(headerOf(record).at(1).contains("SIGSEGV"));

    // A code above 128 that names no signal we know is just a code; so is one naming a signal that other
    // systems number differently (138 is SIGUSR1 on Linux, SIGBUS on macOS and the BSDs).
    CHECK(headerOf(finished(200)).at(1).starts_with("Failed · exit 200"));
    CHECK(headerOf(finished(138)).at(1).starts_with("Failed · exit 138"));
}

TEST_CASE("CommandBlockHeader.anExplicitFailureKeepsItsCode", "[contour][gutter][blockheader]")
{
    // OSC 3008 said the command failed with status 130 and named no signal: that is the program's code.
    auto record = finished(130);
    record.outcome =
        vtbackend::ContextOutcome { .exit = vtbackend::ContextExit::Failure, .signal = {}, .status = 130 };
    CHECK(headerOf(record).at(1).starts_with("Failed · exit 130"));
}

TEST_CASE("CommandBlockHeader.leavesOutWhatTheRecordDoesNotKnow", "[contour][gutter][blockheader]")
{
    auto record = vtbackend::CommandBlockRecord {};
    record.state = vtbackend::CommandBlockState::Running;
    record.commandStartedAt = chrono::system_clock::time_point {};
    CHECK(headerOf(record) == std::vector<std::string> { "Running · 0s", "Started 14:03:07" });

    // A prompt's start stands in for a command's that is not known...
    record.state = vtbackend::CommandBlockState::Finished;
    record.exitCode = 1;
    record.commandStartedAt.reset();
    record.promptStartedAt = chrono::system_clock::time_point {};
    CHECK(headerOf(record).back() == "Started 14:03:07");

    // ...and a record from another process that names neither has no start to show: not the epoch, nor
    // whatever the caller resolved in its place.
    record.promptStartedAt.reset();
    CHECK(headerOf(record) == std::vector<std::string> { "Failed · exit 1" });

    // A prompt still being typed at has run nothing, and so has nothing to say.
    record.state = vtbackend::CommandBlockState::Prompting;
    CHECK(headerOf(record).empty());
}

TEST_CASE("CommandBlockHeader.showsTheCommandLineSanitised", "[contour][gutter][blockheader]")
{
    // Both strings are whatever arrived on the PTY: a title-setting sequence and a bidi override must not
    // reach the header as they are.
    // U+202E RIGHT-TO-LEFT OVERRIDE, spliced in byte by byte rather than written into a literal, where it
    // would be a misleading bidi character itself.
    auto const rightToLeftOverride = std::string { '\xE2', '\x80', '\xAE' };
    auto record = finished(0);
    record.commandLine = vtbackend::CommandLineText { "echo \033]0;owned\007 done" };
    record.workingDirectory.path = "/tmp/" + rightToLeftOverride + "evil";

    for (auto const& line: headerOf(record))
    {
        CHECK_FALSE(line.contains('\033'));
        CHECK_FALSE(line.contains('\007'));
        CHECK_FALSE(line.contains(rightToLeftOverride));
    }
    auto const lines = headerOf(record);
    CHECK(lines.front().contains("\xEF\xBF\xBD")); // U+FFFD in their place
    CHECK(lines.at(2).contains("\xEF\xBF\xBD"));
}
