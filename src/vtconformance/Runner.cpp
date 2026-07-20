// SPDX-License-Identifier: Apache-2.0
#include <vtconformance/Runner.h>

#include <crispy/utils.h>

#include <cstdlib>
#include <deque>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <vtconformance/EscTestLog.h>
#include <vtconformance/VtTestLog.h>

namespace fs = std::filesystem;

namespace vtconformance
{

namespace
{
    [[nodiscard]] std::string readFile(fs::path const& path)
    {
        auto stream = std::ifstream { path, std::ios::binary };
        if (!stream)
            return {};
        return std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    }

    void writeFile(fs::path const& path, std::string_view content)
    {
        auto error = std::error_code {};
        fs::create_directories(path.parent_path(), error);
        auto stream = std::ofstream { path, std::ios::binary | std::ios::trunc };
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    /// Compares a captured dump against its golden, or blesses it.
    ///
    /// @return A human-readable diff when they disagree, else an empty string.
    [[nodiscard]] std::string reconcileGolden(fs::path const& goldenPath,
                                              std::string const& captured,
                                              bool bless)
    {
        if (bless)
        {
            writeFile(goldenPath, captured);
            return {};
        }

        if (!fs::exists(goldenPath))
            return std::format("no golden yet at {} — run with --bless to record one", goldenPath.string());

        auto const diff = diffDumps(readFile(goldenPath), captured);
        if (diff.empty())
            return {};

        return std::format("{}\n{}", goldenPath.filename().string(), diff);
    }

    /// @return Where @p scenario's recorded command file lives, as an absolute path.
    ///
    /// Absolute because the program under test does not share this process's working directory -- it
    /// runs in the scratch directory, or in a fetched suite's own -- so a relative path here means one
    /// thing to the existence check below and another to the child. vttest does not complain when it
    /// cannot open its command file: `setup_replay`'s fopen fails silently, `is_replaying()` is false
    /// forever after (replay.c:35-39), and the very next `readnl()` blocks on a live read that has no
    /// alarm and nobody to answer it. So the symptom of a relative path is a fifteen-minute hang.
    [[nodiscard]] fs::path commandFilePath(RunOptions const& options, Scenario const& scenario)
    {
        return fs::absolute(options.commandDirectory / std::format("{}.cmd", scenario.id));
    }

    /// @return Whether @p transcript records a run that ended in an orderly exit.
    ///
    /// vttest only flushes its log on the way out of `bye()`; SIGTERM and SIGINT merely longjmp back
    /// into its menu loop (main.c:1530-1549), so a force-killed vttest leaves whatever sat in the
    /// stdio buffer unwritten. A truncated transcript holds FEWER verdicts than the run actually
    /// produced -- and every oracle here reads the transcript, so a partial one does not fail, it
    /// silently measures less and reports green. That is the one failure mode a conformance gate may
    /// never have, so the driver checks for vttest's own parting words rather than trusting the file.
    [[nodiscard]] bool exitedCleanly(std::string_view transcript)
    {
        return transcript.find("That's all, folks!") != std::string_view::npos;
    }

    /// Answers one prompt, capturing a golden first when the screen is one a human would have judged.
    void answerPrompt(TerminalHarness& harness,
                      Prompt const& prompt,
                      Scenario const& scenario,
                      RunOptions const& options,
                      std::deque<std::string_view>& keys,
                      size_t step,
                      ScenarioResult& result)
    {
        switch (prompt.action)
        {
            case PromptAction::Hold:
                // The screen is final and is exactly what a human would have been asked to judge, so
                // this is the one place worth capturing.
                if (scenario.kind == ScenarioKind::Visual)
                {
                    auto const goldenPath =
                        options.goldenDirectory / std::format("{}.step{:02}.dump", scenario.id, step);
                    auto const diff = reconcileGolden(goldenPath, harness.dump(), options.bless);
                    if (!diff.empty())
                        result.goldenDiffs.push_back(diff);
                }
                harness.writeInput("\n");
                break;

            case PromptAction::Type: harness.writeInput(prompt.input); break;

            case PromptAction::Key:
                // Through the engine's own input generator, not as bytes: the prompt is asking what
                // the key sends. @see PromptAction::Key.
                harness.engine().pressKey(prompt.key);
                break;

            case PromptAction::Menu:
                // Out of keys means the scenario is done; walk back out of whatever menu we are
                // standing in until vttest exits.
                if (keys.empty())
                    harness.writeInput("0\n");
                else
                {
                    harness.writeInput(std::format("{}\n", keys.front()));
                    keys.pop_front();
                }
                break;
        }
    }

    /// Drives a program by watching its output for prompt markers, answering each until it exits.
    ///
    /// The barrier is causal, and both halves of that are vttest's own doing: `tprintf` flushes stdout
    /// before the read (esc.c:363,371), so the banner IS the fact that it is about to block; and
    /// `holdit()` runs its `inflush()` BEFORE printing the banner (unix_io.c:239-241), so an answer
    /// typed on seeing it can never be the one that gets flushed away.
    ///
    /// It replaced a driver that inferred "the screen is final" from output falling quiet for 150ms and
    /// then scraped the screen. Same policy -- same prompts, same order, same answers -- so every
    /// scenario kept its goldens through the move, which is how the replacement was proven rather than
    /// merely asserted.
    ///
    /// The feed is deliberately sliced: everything up to the marker is the screen the marker announces,
    /// so a golden captured there is the screen a human would have been shown -- not whatever the next
    /// read happened to add to it.
    void driveScanned(TerminalHarness& harness,
                      Suite const& suite,
                      Scenario const& scenario,
                      RunOptions const& options,
                      ScenarioResult& result)
    {
        auto keys = std::deque<std::string_view> { scenario.keys.begin(), scenario.keys.end() };
        auto step = size_t { 0 };
        auto const prompts = suite.prompts;

        auto markers = std::vector<std::string_view> {};
        for (auto const& prompt: prompts)
            markers.push_back(prompt.pattern);
        harness.startScanning(markers);

        auto const deadline = std::chrono::steady_clock::now() + options.runTimeout;

        // The last time the child said anything. A clock belongs here and nowhere else in this loop:
        // deciding WHEN to answer is causal (the marker on the wire), but deciding that a child will
        // NEVER speak again cannot be -- "blocked forever" is only observable by waiting. Keeping the
        // two apart is the whole point; the driver this replaced used one timeout for both, and so
        // sampled screens on a deadline.
        auto lastHeardFrom = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() < deadline)
        {
            auto const batch = harness.readBatch();

            if (batch.bytes.empty() && !batch.closed)
            {
                // A read that came back empty is not silence: `Pty::read` returns EAGAIN the moment it
                // has nothing, including before the child has even been scheduled. Only the clock can
                // say how long it has actually been quiet.
                if (std::chrono::steady_clock::now() - lastHeardFrom < options.harness.stepTimeout)
                    continue;

                if (!harness.alive())
                    return; // It exited; the quiet is just the end.

                // Genuinely blocked on something this table does not answer -- an interactive chapter
                // wanting a keyboard, or a marker that has been missed. Say so now rather than sit
                // here until runTimeout, which is fifteen minutes: vttest's own longest pause is the
                // 5s RIS/DECTST settle, so quiet this long is never merely slowness.
                result.harnessError =
                    std::format("silent for {} after {} step(s) -- blocked on a prompt this suite's "
                                "table does not answer. Screen was:\n{}",
                                options.harness.stepTimeout,
                                step,
                                harness.screenText());
                return;
            }

            lastHeardFrom = std::chrono::steady_clock::now();

            auto offset = size_t { 0 };
            for (auto const& match: batch.matches)
            {
                if (step >= options.maximumSteps)
                {
                    result.harnessError =
                        std::format("gave up after {} steps -- the driver is most likely answering a "
                                    "prompt the program does not accept, and being asked it again. "
                                    "Screen was:\n{}",
                                    step,
                                    harness.screenText());
                    return;
                }

                harness.feed(batch.bytes.substr(offset, match.endOffset - offset));
                offset = match.endOffset;

                answerPrompt(harness, prompts[match.markerIndex], scenario, options, keys, step, result);
                ++step;
            }

            harness.feed(batch.bytes.substr(offset));

            if (batch.closed)
                return; // It exited on its own — that is the normal end of a scenario.
        }

        result.harnessError = std::format("still running after {} and {} step(s). Screen was:\n{}",
                                          options.runTimeout,
                                          step,
                                          harness.screenText());
    }
} // namespace

bool isProgramAvailable(std::string const& program)
{
    if (program.find('/') != std::string::npos)
        return fs::exists(program);

    auto const* const pathEnv = std::getenv("PATH");
    if (!pathEnv)
        return false;

    auto stream = std::istringstream { pathEnv };
    auto directory = std::string {};
    while (std::getline(stream, directory, ':'))
    {
        if (directory.empty())
            continue;
        auto error = std::error_code {};
        if (fs::exists(fs::path(directory) / program, error))
            return true;
    }

    return false;
}

bool isSuiteAvailable(Suite const& suite, fs::path const& suiteDirectory)
{
    if (!isProgramAvailable(std::string(suite.program)))
        return false;

    if (suite.entryPoint.empty())
        return true;

    auto error = std::error_code {};
    return fs::exists(suiteDirectory / suite.entryPoint, error);
}

namespace
{
    /// Drives one scenario from start to finish and records what every oracle saw.
    [[nodiscard]] ScenarioResult runScenario(Suite const& suite,
                                             Scenario const& scenario,
                                             RunOptions const& options)
    {
        auto result = ScenarioResult {
            .id = std::string(scenario.id),
            .title = std::string(scenario.title),
            .kind = scenario.kind,
            .gatesBuild = scenario.gatesBuild,
            .verdicts = {},
            .unansweredQueries = {},
            .diagnostics = {},
            .goldenDiffs = {},
            .escTest = {},
            .harnessError = {},
        };

        // An interactive scenario is still RUN, it is just never judged (see `judge()`).
        //
        // It cannot be driven to completion -- a headless engine has no keyboard -- so it will wall
        // up partway through and be reported as skipped. But everything it manages to send before
        // then still passes through the engine, and oracle A is watching. Chapter 3's charset items
        // run before its soft-character item asks for a key press, so declining to spawn it at all
        // would throw away the entire character-set gap list.
        //
        // Its walk is bounded twice over: by the step budget, and by the step timeout it hits the
        // moment vttest asks for something the driver cannot supply.
        // Absolute, for the same reason the command file is: the child writes this from a working
        // directory that is not ours, so a relative path would have it write one file while we read
        // another. @see commandFilePath.
        auto const logPath = fs::absolute(options.workDirectory / std::format("{}.log", scenario.id));
        auto error = std::error_code {};
        fs::create_directories(options.workDirectory, error);
        fs::remove(logPath, error);

        auto arguments = suite.arguments;
        arguments.emplace_back(suite.logFlag);
        arguments.emplace_back(logPath.string());

        // Recording a command file means driving the scenario ourselves and keeping what vttest wrote;
        // replaying means handing that recording straight back to it. The recorder is the same Live
        // driver everything else uses, so a recording is as reproducible as a run -- which it was not
        // when this had to fall back to the timing-based driver, and a command file blessed from a
        // beat sampled wrong would have been baked into the baseline forever.
        auto const driveMode = options.blessCommandFiles && scenario.driveMode == DriveMode::Replay
                                   ? DriveMode::Live
                                   : scenario.driveMode;

        auto const commandPath = commandFilePath(options, scenario);
        if (driveMode == DriveMode::Replay)
        {
            if (!fs::exists(commandPath))
            {
                result.harnessError = std::format(
                    "no command file at {} -- record one with --bless-command-files", commandPath.string());
                return result;
            }
            arguments.emplace_back(suite.cmdFlag);
            arguments.emplace_back(commandPath.string());
        }

        if (!options.testFilter.empty() && !suite.testFilterFlag.empty())
        {
            arguments.emplace_back(suite.testFilterFlag);
            arguments.emplace_back(options.testFilter);
        }

        auto harnessOptions = options.harness;
        harnessOptions.engine.pageSize = suite.pageSize;
        harnessOptions.engine.checksumExtension = suite.checksumExtension;

        auto exec = vtpty::Process::ExecInfo {
            .program = options.program.empty() ? std::string(suite.program) : options.program,
            .arguments = arguments,
            // A fetched suite runs from its own directory, so that its entry point and its imports
            // resolve; an installed one runs from the scratch directory.
            .workingDirectory = suite.entryPoint.empty() ? options.workDirectory : options.suiteDirectory,
            // vttest inspects the locale to decide whether to switch the terminal out of UTF-8.
            // Pinning it keeps the byte stream — and therefore the goldens — reproducible.
            .env = { { "TERM", "xterm-256color" }, { "LC_ALL", "C" } },
        };

        auto harness = TerminalHarness { exec, harnessOptions };

        // A scanned run reads the device itself, so it must be the only reader. @see Pumping.
        harness.start(driveMode == DriveMode::Live ? TerminalHarness::Pumping::ByTheCaller
                                                   : TerminalHarness::Pumping::Internally);

        // An unattended suite needs no driving at all: it walks its own test list and exits. There is
        // nothing to type at it, and so nothing to mistype -- which is exactly why a self-asserting
        // suite is worth more per test than a menu-driven one.
        if (driveMode == DriveMode::Unattended)
        {
            if (!harness.waitForExit(options.runTimeout))
                result.harnessError = std::format(
                    "still running after {}. Screen was:\n{}", options.runTimeout, harness.screenText());

            harness.stop();
            result.diagnostics = harness.diagnostics();
            result.escTest = parseEscTestLog(readFile(logPath));
            return result;
        }

        // A replayed scenario is unattended too, and for the same reason: vttest reads its keystrokes
        // from the command file, so the driver types nothing and never looks at the screen. That is
        // the whole point -- there is no barrier to sample early and no menu to mis-navigate, so the
        // run is reproducible rather than merely usually-right.
        if (driveMode == DriveMode::Replay)
        {
            if (!harness.waitForExit(options.runTimeout))
                result.harnessError =
                    std::format("still running after {} -- the command file has most likely run short, "
                                "leaving vttest blocked on a read it can no longer satisfy. Screen "
                                "was:\n{}",
                                options.runTimeout,
                                harness.screenText());

            harness.stop();
            result.diagnostics = harness.diagnostics();

            auto const transcript = readFile(logPath);
            if (result.harnessError.empty() && !exitedCleanly(transcript))
                result.harnessError =
                    std::format("vttest did not exit cleanly, so its transcript ({}) is truncated and "
                                "its verdicts cannot be trusted. Screen was:\n{}",
                                logPath.string(),
                                harness.screenText());

            auto const records = parseVtTestLog(transcript);
            result.verdicts = extractVerdicts(records);
            for (auto& query: extractQueries(records))
                if (!query.answered())
                    result.unansweredQueries.push_back(std::move(query));

            return result;
        }

        driveScanned(harness, suite, scenario, options, result);

        harness.stop();
        result.diagnostics = harness.diagnostics();

        auto const transcript = readFile(logPath);
        auto const records = parseVtTestLog(transcript);
        result.verdicts = extractVerdicts(records);

        for (auto& query: extractQueries(records))
            if (!query.answered())
                result.unansweredQueries.push_back(std::move(query));

        // Recording: vttest's transcript IS its command-file format, so the run just driven is the
        // recording. Only an orderly exit may be recorded -- a truncated transcript would replay
        // short, leaving vttest blocked on a read the file can no longer answer, and would bake this
        // one flaky run into the baseline forever.
        if (options.blessCommandFiles && scenario.driveMode == DriveMode::Replay)
        {
            if (!exitedCleanly(transcript))
                result.harnessError =
                    std::format("refusing to record a command file from a run that did not exit "
                                "cleanly -- its transcript is truncated. Screen was:\n{}",
                                harness.screenText());
            else
                writeFile(commandFilePath(options, scenario), transcript);
        }

        return result;
    }
} // namespace

Report runSuite(Suite const& suite, RunOptions const& options)
{
    auto report =
        Report { .suite = std::string(suite.name), .scenarios = {}, .knownGaps = {}, .knownFailures = {} };

    if (!options.knownGapsFile.empty())
        report.knownGaps = Ratchet::parse(readFile(options.knownGapsFile));

    if (!options.knownFailuresFile.empty())
        report.knownFailures = Ratchet::parse(readFile(options.knownFailuresFile));

    for (auto const& scenario: suite.scenarios)
    {
        if (options.filter && scenario.id.find(*options.filter) == std::string_view::npos)
            continue;
        report.scenarios.push_back(runScenario(suite, scenario, options));
    }

    if (options.updateKnownGaps && !options.knownGapsFile.empty())
    {
        auto refreshed = Ratchet {};
        for (auto const& diagnostic: allDiagnostics(report))
            refreshed.entries.emplace(gapKey(diagnostic));
        writeFile(options.knownGapsFile, refreshed.render(KnownGapsHeader));
        report.knownGaps = refreshed;
    }

    if (options.updateKnownFailures && !options.knownFailuresFile.empty())
    {
        auto refreshed = Ratchet {};
        for (auto const& scenario: report.scenarios)
            refreshed.entries.insert(scenario.escTest.failingTests.begin(),
                                     scenario.escTest.failingTests.end());
        writeFile(options.knownFailuresFile, refreshed.render(KnownFailuresHeader));
        report.knownFailures = refreshed;
    }

    return report;
}

} // namespace vtconformance
