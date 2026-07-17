// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include <vtconformance/Report.h>

using namespace std::string_view_literals;
using namespace vtconformance;

namespace
{
[[nodiscard]] Diagnostic gap(std::string sequence, DiagnosticKind kind = DiagnosticKind::Unknown)
{
    return Diagnostic { .kind = kind, .sequence = std::move(sequence), .count = 1 };
}

[[nodiscard]] ScenarioResult scenario(std::string id, ScenarioKind kind = ScenarioKind::SelfChecking)
{
    return ScenarioResult { .id = std::move(id),
                            .title = "a test",
                            .kind = kind,
                            .verdicts = {},
                            .unansweredQueries = {},
                            .diagnostics = {},
                            .goldenDiffs = {},
                            .escTest = {},
                            .harnessError = {} };
}
} // namespace

TEST_CASE("Report.Ratchet.parse", "[vtconformance]")
{
    auto const gaps = Ratchet::parse("# a comment\n"
                                     "\n"
                                     "unknown: ESC Z\n"
                                     "  unsupported: CSI 4 i  \n"sv);

    CHECK(gaps.entries.size() == 2);
    CHECK(gaps.entries.contains("unknown: ESC Z"));
    CHECK(gaps.entries.contains("unsupported: CSI 4 i"));
}

TEST_CASE("Report.Ratchet round-trips through render/parse", "[vtconformance]")
{
    auto original = Ratchet {};
    original.entries.emplace("unknown: ESC Z");
    original.entries.emplace("invalid: CSI 99 \" p");

    CHECK(Ratchet::parse(original.render()).entries == original.entries);
}

TEST_CASE("Report.judge", "[vtconformance]")
{
    SECTION("a clean scenario passes")
    {
        CHECK(judge(scenario("clean"), Ratchet {}) == Outcome::Passed);
    }

    SECTION("an interactive scenario is skipped, never passed")
    {
        // Reporting an undriveable test as green would be the single most misleading thing this
        // harness could do.
        auto result = scenario("keyboard", ScenarioKind::Interactive);
        CHECK(judge(result, Ratchet {}) == Outcome::Skipped);
    }

    SECTION("a failing self-check fails the scenario")
    {
        auto result = scenario("reports");
        result.verdicts.push_back(Verdict { .text = "-- Not expected", .passed = false, .lineNumber = 1 });
        CHECK(judge(result, Ratchet {}) == Outcome::Failed);
    }

    SECTION("an unanswered query fails the scenario")
    {
        auto result = scenario("reports");
        result.unansweredQueries.push_back(
            Query { .request = "\033P$q\"p\033\\", .reply = {}, .lineNumber = 1 });
        CHECK(judge(result, Ratchet {}) == Outcome::Failed);
    }

    SECTION("a golden mismatch fails the scenario")
    {
        auto result = scenario("cursor", ScenarioKind::Visual);
        result.goldenDiffs.emplace_back("@@ line 3\n-x\n+y\n");
        CHECK(judge(result, Ratchet {}) == Outcome::Failed);
    }

    SECTION("a NEW ignored sequence fails the scenario")
    {
        auto result = scenario("charsets");
        result.diagnostics.push_back(gap("ESC Z"));
        CHECK(judge(result, Ratchet {}) == Outcome::Failed);
    }

    SECTION("a RATCHETED ignored sequence does not fail the scenario")
    {
        // The ratchet is what lets the gate be blocking on the day it lands: a known gap is tolerated
        // so the build is green, but the moment a NEW one appears the build breaks.
        auto result = scenario("charsets");
        result.diagnostics.push_back(gap("ESC Z"));

        auto known = Ratchet {};
        known.entries.emplace("unknown: ESC Z");

        CHECK(judge(result, known) == Outcome::Passed);
    }

    SECTION("a harness error fails the scenario")
    {
        auto result = scenario("wedged");
        result.harnessError = "timed out waiting for a prompt";
        CHECK(judge(result, Ratchet {}) == Outcome::Failed);
    }
}

TEST_CASE("Report.staleGaps", "[vtconformance]")
{
    // A ratchet entry that no longer occurs means the gap was fixed. Leaving it in place would let
    // the entry silently start covering some *other*, future regression — so it is itself an error.
    auto report = Report { .suite = "vttest", .scenarios = {}, .knownGaps = {}, .knownFailures = {} };
    report.knownGaps.entries.emplace("unknown: ESC Z");
    report.knownGaps.entries.emplace("unknown: ESC Q");

    auto result = scenario("charsets");
    result.diagnostics.push_back(gap("ESC Z"));
    report.scenarios.push_back(result);

    auto const stale = staleGaps(report);
    REQUIRE(stale.size() == 1);
    CHECK(stale[0] == "unknown: ESC Q");

    CHECK(exitCode(report) == EXIT_FAILURE);
}

TEST_CASE("Report.allDiagnostics merges counts across scenarios", "[vtconformance]")
{
    auto report = Report { .suite = "vttest", .scenarios = {}, .knownGaps = {}, .knownFailures = {} };

    auto first = scenario("one");
    first.diagnostics.push_back(
        Diagnostic { .kind = DiagnosticKind::Unknown, .sequence = "ESC Z", .count = 2 });
    report.scenarios.push_back(first);

    auto second = scenario("two");
    second.diagnostics.push_back(
        Diagnostic { .kind = DiagnosticKind::Unknown, .sequence = "ESC Z", .count = 3 });
    report.scenarios.push_back(second);

    auto const merged = allDiagnostics(report);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].count == 5);
}

TEST_CASE("Report.exitCode", "[vtconformance]")
{
    SECTION("a run with only passes and skips succeeds")
    {
        auto report = Report { .suite = "vttest", .scenarios = {}, .knownGaps = {}, .knownFailures = {} };
        report.scenarios.push_back(scenario("clean"));
        report.scenarios.push_back(scenario("keyboard", ScenarioKind::Interactive));

        CHECK(exitCode(report) == EXIT_SUCCESS);
    }

    SECTION("a run with a failure fails")
    {
        auto report = Report { .suite = "vttest", .scenarios = {}, .knownGaps = {}, .knownFailures = {} };
        auto result = scenario("reports");
        result.gatesBuild = true;
        result.harnessError = "boom";
        report.scenarios.push_back(result);

        CHECK(exitCode(report) == EXIT_FAILURE);
    }

    SECTION("a failing scenario that does not gate is reported, not fatal")
    {
        // Whether a scenario gates is a property of the scenario (@see Scenario::gatesBuild), and the
        // suites do set it per chapter. A non-gating one must still be surfaced rather than swallowed,
        // which is what this pins -- exitCode() reads the flag and does not second-guess it.
        auto report = Report { .suite = "vttest", .scenarios = {}, .knownGaps = {}, .knownFailures = {} };
        auto result = scenario("cursor-movements");
        result.gatesBuild = false;
        result.harnessError = "boom";
        report.scenarios.push_back(result);

        CHECK(judge(report.scenarios[0], report.knownGaps, report.knownFailures) == Outcome::Failed);
        CHECK(exitCode(report) == EXIT_SUCCESS);
        // ... and it must still be visible: a failure nobody can see is a failure nobody will fix.
        CHECK(renderSummary(report).find("advisory") != std::string::npos);
    }

    SECTION("one gating failure fails the run even beside passing advisory scenarios")
    {
        auto report = Report { .suite = "vttest", .scenarios = {}, .knownGaps = {}, .knownFailures = {} };
        report.scenarios.push_back(scenario("cursor-movements")); // advisory, passing
        auto gating = scenario("reports");
        gating.gatesBuild = true;
        gating.harnessError = "boom";
        report.scenarios.push_back(gating);

        CHECK(exitCode(report) == EXIT_FAILURE);
    }
}

TEST_CASE("Report.renderMarkdown", "[vtconformance]")
{
    auto report = Report { .suite = "vttest", .scenarios = {}, .knownGaps = {}, .knownFailures = {} };
    report.scenarios.push_back(scenario("vttest.06.terminal-reports"));

    auto result = scenario("vttest.05.keyboard", ScenarioKind::Interactive);
    report.scenarios.push_back(result);

    auto const markdown = renderMarkdown(report);
    CHECK(markdown.find("### Suite: `vttest`") != std::string::npos);
    CHECK(markdown.find("1 passed, 0 failed, 1 skipped") != std::string::npos);
    CHECK(markdown.find("needs real key/mouse input") != std::string::npos);
}
