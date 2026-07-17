// SPDX-License-Identifier: Apache-2.0
#include <crispy/App.h>
#include <crispy/CLI.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

#include <vtconformance/Report.h>
#include <vtconformance/Runner.h>
#include <vtconformance/Suite.h>

namespace fs = std::filesystem;
namespace CLI = crispy::cli;

using namespace vtconformance;
using namespace std::string_literals;

namespace
{

/// The conformance harness CLI.
///
/// It drives an external VT test program (vttest today) against Contour's own terminal engine over a
/// real PTY, entirely headlessly, and reports what every oracle saw.
class ConformanceApp: public crispy::app
{
  public:
    ConformanceApp(): crispy::app("vtconformance", "Contour VT conformance harness", "0.1.0", "Apache-2.0")
    {
        link("vtconformance.run", [this] { return runCommand(); });
    }

    [[nodiscard]] CLI::command parameterDefinition() const override
    {
        return CLI::command {
            "vtconformance",
            "Runs a VT conformance suite against Contour's terminal engine and reports the result.",
            CLI::option_list {},
            CLI::command_list {
                // crispy::app links help/version/license handlers in its constructor and looks every
                // linked handler's key up when dispatching, so these must be declared here or the
                // lookup throws before main() ever gets a say.
                CLI::command { "help", "Shows this help and exits." },
                CLI::command { "version", "Shows the version and exits." },
                CLI::command { "license", "Shows the license and exits." },
                CLI::command {
                    "run",
                    "Runs a conformance suite headlessly and reports what every oracle saw.",
                    CLI::option_list {
                        CLI::option { "suite", CLI::value { "vttest"s }, "Which suite to run." },
                        CLI::option { "program",
                                      CLI::value { ""s },
                                      "Overrides the suite's test program (e.g. a locally built vttest)." },
                        CLI::option {
                            "golden-dir", CLI::value { ""s }, "Directory holding blessed screen dumps." },
                        CLI::option { "known-gaps", CLI::value { ""s }, "The known-gap ratchet file." },
                        CLI::option {
                            "work-dir", CLI::value { ""s }, "Where suite transcripts are written." },
                        CLI::option { "filter",
                                      CLI::value { ""s },
                                      "Runs only scenarios whose id contains this text." },
                        CLI::option { "test-filter",
                                      CLI::value { ""s },
                                      "Runs only the suite's own tests matching this (esctest)." },
                        CLI::option {
                            "markdown", CLI::value { ""s }, "Also writes a Markdown report to this file." },
                        CLI::option { "bless",
                                      CLI::value { false },
                                      "Records the captured screens as the new goldens." },
                        CLI::option { "update-known-gaps",
                                      CLI::value { false },
                                      "Rewrites the ratchet from what this run observed." },
                        CLI::option {
                            "known-failures", CLI::value { ""s }, "The esctest failure ratchet file." },
                        CLI::option { "update-known-failures",
                                      CLI::value { false },
                                      "Rewrites the failure ratchet from what this run observed." },
                        CLI::option {
                            "suite-dir", CLI::value { ""s }, "Where a fetched suite (esctest) lives." },
                        CLI::option { "skip-if-missing",
                                      CLI::value { false },
                                      "Exits successfully when the test program is not installed." },
                        CLI::option { "command-dir",
                                      CLI::value { ""s },
                                      "Directory holding the recorded command files a replayed "
                                      "scenario is driven by." },
                        CLI::option { "bless-command-files",
                                      CLI::value { false },
                                      "Records each replayed scenario's command file instead of "
                                      "replaying it." },
                    },
                },
            },
        };
    }

  private:
    [[nodiscard]] int runCommand()
    {
        auto const suiteName = parameters().str("vtconformance.run.suite");
        auto const* const found = findSuite(suiteName);
        if (!found)
        {
            std::cerr << std::format("Unknown suite: {}\n", suiteName);
            return EXIT_FAILURE;
        }

        auto const& suite = *found;

        auto options = RunOptions {};
        options.program = parameters().str("vtconformance.run.program");
        options.bless = parameters().boolean("vtconformance.run.bless");
        options.updateKnownGaps = parameters().boolean("vtconformance.run.update-known-gaps");
        options.updateKnownFailures = parameters().boolean("vtconformance.run.update-known-failures");
        options.blessCommandFiles = parameters().boolean("vtconformance.run.bless-command-files");

        if (auto const value = parameters().str("vtconformance.run.command-dir"); !value.empty())
            options.commandDirectory = fs::path(value);

        if (auto const value = parameters().str("vtconformance.run.suite-dir"); !value.empty())
            options.suiteDirectory = fs::path(value);

        if (!isSuiteAvailable(suite, options.suiteDirectory))
        {
            auto const message = suite.entryPoint.empty()
                                     ? std::format("Test program '{}' is not installed.\n", suite.program)
                                     : std::format("Suite '{}' was not found. Fetch it first "
                                                   "(scripts/fetch-esctest.sh), then pass --suite-dir.\n",
                                                   suite.name);
            if (parameters().boolean("vtconformance.run.skip-if-missing"))
            {
                // A gate that cannot find its test program has measured nothing. Say so loudly, but
                // do not fail a build that never promised to have the suite installed.
                std::cerr << message << "Skipping the conformance run.\n";
                return EXIT_SUCCESS;
            }
            std::cerr << message;
            return EXIT_FAILURE;
        }

        if (auto const value = parameters().str("vtconformance.run.golden-dir"); !value.empty())
            options.goldenDirectory = fs::path(value);

        if (auto const value = parameters().str("vtconformance.run.known-gaps"); !value.empty())
            options.knownGapsFile = fs::path(value);

        if (auto const value = parameters().str("vtconformance.run.known-failures"); !value.empty())
            options.knownFailuresFile = fs::path(value);

        if (auto const value = parameters().str("vtconformance.run.filter"); !value.empty())
            options.filter = value;

        options.testFilter = parameters().str("vtconformance.run.test-filter");

        auto const workDirectory = parameters().str("vtconformance.run.work-dir");
        options.workDirectory =
            workDirectory.empty() ? fs::temp_directory_path() / "vtconformance" : fs::path(workDirectory);

        auto const report = runSuite(suite, options);

        std::cout << renderSummary(report);

        if (auto const value = parameters().str("vtconformance.run.markdown"); !value.empty())
        {
            auto stream = std::ofstream { value, std::ios::trunc };
            stream << renderMarkdown(report);
        }

        // Blessing and ratchet-updating are recording modes, not judging modes: they always succeed,
        // because their whole point is to change what "correct" means.
        if (options.bless || options.updateKnownGaps || options.updateKnownFailures
            || options.blessCommandFiles)
            return EXIT_SUCCESS;

        return exitCode(report);
    }
};

} // namespace

int main(int argc, char const* argv[])
{
    auto app = ConformanceApp {};
    return app.run(argc, argv);
}
