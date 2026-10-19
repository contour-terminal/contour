// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <vtconformance/Report.hpp>
#include <vtconformance/Suite.hpp>
#include <vtconformance/TerminalHarness.hpp>

namespace vtconformance
{

/// How to run a suite.
struct RunOptions
{
    /// Where blessed screen dumps live.
    std::filesystem::path goldenDirectory;

    /// Overwrite goldens with what was captured, instead of comparing against them.
    ///
    /// A blessed golden is a claim about correct VT behaviour, so blessing is deliberately a
    /// separate, explicit act — never a side effect of a failing run.
    bool bless = false;

    /// The known-gap ratchet file.
    std::filesystem::path knownGapsFile;

    /// Rewrite the ratchet from what this run observed.
    bool updateKnownGaps = false;

    /// Overrides the suite's program (e.g. an absolute path to a locally built vttest).
    std::string program;

    /// Runs only scenarios whose id contains this substring.
    std::optional<std::string> filter;

    /// Passed to the program itself, to run only some of its own tests (esctest's `--include`).
    std::string testFilter;

    /// Terminal geometry and timing.
    TerminalHarness::Options harness {};

    /// How long a program may run before it is declared wedged.
    ///
    /// Generous, because esctest runs hundreds of tests and each unanswered query costs it its own
    /// read timeout -- a terminal missing a report makes the whole run slower, not just that one test.
    std::chrono::milliseconds runTimeout { 900000 };

    /// How many prompts a single scenario may answer before the driver gives up.
    ///
    /// A prompt-reactive driver can livelock: answer a prompt the program does not accept, and it
    /// redraws the same prompt, which looks like fresh output and provokes the same answer forever. A
    /// budget turns that into a bounded, legible failure instead of a hang.
    size_t maximumSteps = 400;

    /// Where the suite's own transcripts are written. Defaults to a temporary directory.
    std::filesystem::path workDirectory;

    /// Where a fetched suite lives, when it is not installed on PATH.
    ///
    /// esctest is GPL-2.0 and Contour is Apache-2.0, so it is cloned rather than vendored
    /// (`scripts/fetch-esctest.sh`). The program runs with this as its working directory.
    std::filesystem::path suiteDirectory;

    /// The esctest failure ratchet: tests known to fail, one per line.
    std::filesystem::path knownFailuresFile;

    /// Rewrite the failure ratchet from what this run observed.
    bool updateKnownFailures = false;

    /// Where the recorded command files that drive `DriveMode::Replay` live.
    std::filesystem::path commandDirectory;

    /// Record each scenario's command file instead of replaying it.
    ///
    /// Blessing a command file drives the scenario with `DriveMode::Live` and keeps the transcript vttest
    /// wrote, which *is* a valid command file. The recorder is therefore the same marker-driven driver
    /// every other scenario uses -- a recording is as reproducible as a run. It is still a deliberate,
    /// explicit act for the same reason blessing a golden is: it changes what the suite will do forever
    /// after.
    bool blessCommandFiles = false;
};

/// Runs every scenario of @p suite and collects what all oracles saw.
///
/// @return The report. It is a value: rendering and judging it are pure functions.
[[nodiscard]] Report runSuite(Suite const& suite, RunOptions const& options);

/// @return Whether @p program can be found and executed.
[[nodiscard]] bool isProgramAvailable(std::string const& program);

/// @return The program a run of @p suite executes: the override in @p options when it names one, else the
///         suite's own.
[[nodiscard]] std::string programOf(Suite const& suite, RunOptions const& options);

/// Tells whether a program can be found and executed. @see isProgramAvailable, the one a run asks.
using ProgramProbe = std::function<bool(std::string const&)>;

/// @return Whether @p suite can be run at all as @p options would run it: the program it executes
///         (programOf) can be found, as @p isAvailable says, and, if the suite has one, its entry point is
///         present in the suite directory of @p options.
///
/// A suite that has not been fetched must degrade to a skip, never to a failure -- a red gate that
/// merely means "you did not clone a GPL repository" teaches everyone to ignore the gate.
/// @param suite The suite to run.
/// @param options How it would run.
/// @param isAvailable Asked about the program the run executes.
[[nodiscard]] bool isSuiteAvailable(Suite const& suite,
                                    RunOptions const& options,
                                    ProgramProbe const& isAvailable);

} // namespace vtconformance
