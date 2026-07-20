// SPDX-License-Identifier: Apache-2.0
#include <vtconformance/Report.h>

#include <crispy/utils.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

using namespace std::string_view_literals;

namespace vtconformance
{

namespace
{
    constexpr auto DiagnosticNames = std::array {
        std::pair { DiagnosticKind::Unknown, "unknown"sv },
        std::pair { DiagnosticKind::Unsupported, "unsupported"sv },
        std::pair { DiagnosticKind::Invalid, "invalid"sv },
        std::pair { DiagnosticKind::ParserError, "parser-error"sv },
    };

    [[nodiscard]] std::string_view nameOf(DiagnosticKind kind) noexcept
    {
        for (auto const& [candidate, name]: DiagnosticNames)
            if (candidate == kind)
                return name;
        return "?"sv;
    }

    [[nodiscard]] std::string_view nameOf(Outcome outcome) noexcept
    {
        switch (outcome)
        {
            case Outcome::Passed: return "PASS"sv;
            case Outcome::Failed: return "FAIL"sv;
            case Outcome::Skipped: return "SKIP"sv;
        }
        return "?"sv;
    }

    [[nodiscard]] std::string_view nameOf(ScenarioKind kind) noexcept
    {
        switch (kind)
        {
            case ScenarioKind::SelfChecking: return "self-checking"sv;
            case ScenarioKind::Visual: return "visual"sv;
            case ScenarioKind::Interactive: return "interactive"sv;
        }
        return "?"sv;
    }

} // namespace

std::string gapKey(Diagnostic const& diagnostic)
{
    return std::format("{}: {}", nameOf(diagnostic.kind), diagnostic.sequence);
}

Ratchet Ratchet::parse(std::string_view text)
{
    auto gaps = Ratchet {};

    for (auto line: crispy::split(text, '\n'))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        while (!line.empty() && line.front() == ' ')
            line.remove_prefix(1);

        if (line.empty() || line.front() == '#')
            continue;

        gaps.entries.emplace(line);
    }

    return gaps;
}

std::string Ratchet::render(std::string_view header) const
{
    auto out = std::ostringstream {};
    out << header;

    for (auto const& entry: entries)
        out << entry << '\n';

    return out.str();
}

Outcome judge(ScenarioResult const& result, Ratchet const& knownGaps, Ratchet const& knownFailures)
{
    if (result.kind == ScenarioKind::Interactive)
        return Outcome::Skipped;

    if (!result.harnessError.empty())
        return Outcome::Failed;

    if (std::ranges::any_of(result.verdicts, [](Verdict const& v) { return !v.passed; }))
        return Outcome::Failed;

    if (!result.unansweredQueries.empty())
        return Outcome::Failed;

    if (!result.goldenDiffs.empty())
        return Outcome::Failed;

    // A self-asserting suite that ran but printed no verdict is a failure, not a pass: it crashed,
    // and a crash that reports nothing must not read as "nothing was wrong".
    if (!result.escTest.valid() && result.escTest.total() == 0 && !result.escTest.parseError.empty())
        return Outcome::Failed;

    auto const unratchetedFailure = [&](std::string const& test) {
        return !knownFailures.entries.contains(test);
    };
    if (std::ranges::any_of(result.escTest.failingTests, unratchetedFailure))
        return Outcome::Failed;

    auto const unratchetedGap = [&](Diagnostic const& diagnostic) {
        return !knownGaps.entries.contains(gapKey(diagnostic));
    };
    if (std::ranges::any_of(result.diagnostics, unratchetedGap))
        return Outcome::Failed;

    return Outcome::Passed;
}

std::vector<std::string> newFailures(Report const& report)
{
    auto fresh = std::vector<std::string> {};
    for (auto const& scenario: report.scenarios)
        for (auto const& test: scenario.escTest.failingTests)
            if (!report.knownFailures.entries.contains(test))
                fresh.push_back(test);
    std::ranges::sort(fresh);
    return fresh;
}

std::vector<std::string> staleFailures(Report const& report)
{
    auto failing = std::set<std::string> {};
    for (auto const& scenario: report.scenarios)
        failing.insert(scenario.escTest.failingTests.begin(), scenario.escTest.failingTests.end());

    // A ratchet entry that no longer fails must be removed, or the ratchet stops describing reality
    // and starts hiding the next regression behind itself.
    auto stale = std::vector<std::string> {};
    for (auto const& test: report.knownFailures.entries)
        if (!failing.contains(test))
            stale.push_back(test);
    return stale;
}

std::vector<Diagnostic> allDiagnostics(Report const& report)
{
    auto merged = std::vector<Diagnostic> {};

    for (auto const& scenario: report.scenarios)
        for (auto const& diagnostic: scenario.diagnostics)
        {
            auto const existing = std::ranges::find_if(merged, [&](Diagnostic const& candidate) {
                return candidate.kind == diagnostic.kind && candidate.sequence == diagnostic.sequence;
            });
            if (existing != merged.end())
                existing->count += diagnostic.count;
            else
                merged.push_back(diagnostic);
        }

    std::ranges::sort(merged, [](Diagnostic const& a, Diagnostic const& b) {
        if (a.kind != b.kind)
            return a.kind < b.kind;
        return a.sequence < b.sequence;
    });

    return merged;
}

std::vector<std::string> staleGaps(Report const& report)
{
    auto seen = std::set<std::string> {};
    for (auto const& diagnostic: allDiagnostics(report))
        seen.insert(gapKey(diagnostic));

    auto stale = std::vector<std::string> {};
    for (auto const& gap: report.knownGaps.entries)
        if (!seen.contains(gap))
            stale.push_back(gap);

    return stale;
}

std::string renderSummary(Report const& report)
{
    auto out = std::ostringstream {};
    out << std::format("Conformance suite: {}\n\n", report.suite);

    for (auto const& scenario: report.scenarios)
    {
        auto const outcome = judge(scenario, report.knownGaps, report.knownFailures);
        out << std::format("  {}  {:<32} {}\n", nameOf(outcome), scenario.id, scenario.title);

        // Say so, rather than letting a red line in the log imply a red build: a reader who cannot
        // tell an advisory failure from a gating one learns to distrust both. @see Scenario::gatesBuild.
        if (outcome == Outcome::Failed && !scenario.gatesBuild)
            out << "        (advisory — reported, does not gate the build)\n";

        if (!scenario.harnessError.empty())
            out << std::format("        ! {}\n", scenario.harnessError);

        for (auto const& verdict: scenario.verdicts)
            if (!verdict.passed)
                out << std::format("        - {}\n", verdict.text);

        for (auto const& query: scenario.unansweredQueries)
            out << std::format("        - unanswered query: {}\n", prettyBytes(query.request));

        for (auto const& diff: scenario.goldenDiffs)
            out << std::format("        - golden mismatch:\n{}\n", diff);

        if (!scenario.escTest.parseError.empty())
            out << std::format("        ! {}\n", scenario.escTest.parseError);
        else if (scenario.escTest.total())
            out << std::format("        {} passed, {} known bugs, {} failed\n",
                               scenario.escTest.passed,
                               scenario.escTest.knownBugs,
                               scenario.escTest.failed);

        for (auto const& test: scenario.escTest.failingTests)
            out << std::format(
                "        {} {}\n", report.knownFailures.entries.contains(test) ? "[known]" : "[NEW! ]", test);
    }

    auto const staleTests = staleFailures(report);
    if (!staleTests.empty()
        && std::ranges::any_of(report.scenarios, [](auto const& s) { return s.escTest.valid(); }))
    {
        out << std::format("\nStale failure-ratchet entries ({}) — these tests now pass; remove them:\n",
                           staleTests.size());
        for (auto const& test: staleTests)
            out << std::format("  {}\n", test);
    }

    auto const gaps = allDiagnostics(report);
    out << std::format("\nIgnored sequences: {}\n", gaps.size());
    for (auto const& gap: gaps)
    {
        auto const key = gapKey(gap);
        auto const known = report.knownGaps.entries.contains(key);
        out << std::format("  {} {} (x{})\n", known ? "[known]" : "[NEW! ]", key, gap.count);
    }

    auto const stale = staleGaps(report);
    if (!stale.empty())
    {
        out << std::format("\nStale ratchet entries ({}) — these gaps appear fixed; remove them:\n",
                           stale.size());
        for (auto const& gap: stale)
            out << std::format("  {}\n", gap);
    }

    return out.str();
}

std::string renderMarkdown(Report const& report)
{
    auto out = std::ostringstream {};

    auto passed = size_t { 0 };
    auto failed = size_t { 0 };
    auto skipped = size_t { 0 };
    for (auto const& scenario: report.scenarios)
        switch (judge(scenario, report.knownGaps, report.knownFailures))
        {
            case Outcome::Passed: ++passed; break;
            case Outcome::Failed: ++failed; break;
            case Outcome::Skipped: ++skipped; break;
        }

    out << std::format("### Suite: `{}`\n\n", report.suite);
    out << std::format("**{} passed, {} failed, {} skipped** out of {} scenarios.\n\n",
                       passed,
                       failed,
                       skipped,
                       report.scenarios.size());

    out << "| Scenario | Kind | Outcome | Notes |\n";
    out << "|---|---|---|---|\n";
    for (auto const& scenario: report.scenarios)
    {
        auto const outcome = judge(scenario, report.knownGaps, report.knownFailures);
        auto notes = std::vector<std::string> {};

        if (!scenario.harnessError.empty())
            notes.push_back(scenario.harnessError);

        auto const failedVerdicts = static_cast<size_t>(
            std::ranges::count_if(scenario.verdicts, [](Verdict const& v) { return !v.passed; }));
        if (failedVerdicts)
            notes.push_back(std::format("{} failing self-check(s)", failedVerdicts));

        if (!scenario.unansweredQueries.empty())
            notes.push_back(std::format("{} unanswered quer(y/ies)", scenario.unansweredQueries.size()));

        if (!scenario.goldenDiffs.empty())
            notes.push_back(std::format("{} golden mismatch(es)", scenario.goldenDiffs.size()));

        if (scenario.kind == ScenarioKind::Interactive)
            notes.emplace_back("needs real key/mouse input");

        // Say so, rather than letting a red line in the log imply a red build. A reader who cannot
        // tell an advisory failure from a gating one learns to distrust both.
        if (outcome == Outcome::Failed && !scenario.gatesBuild)
            notes.emplace_back("advisory — does not gate the build");

        auto joined = std::string {};
        for (auto const& [index, note]: crispy::views::enumerate(notes))
            joined += index ? std::format("; {}", note) : note;

        out << std::format("| `{}` | {} | {} | {} |\n",
                           scenario.id,
                           nameOf(scenario.kind),
                           nameOf(outcome),
                           joined.empty() ? "—" : joined);
    }

    auto const gaps = allDiagnostics(report);
    out << std::format("\n### Ignored sequences ({})\n\n", gaps.size());
    if (gaps.empty())
        out << "None. Every sequence the suite sent was honoured.\n";
    else
    {
        out << "| Kind | Sequence | Occurrences | Ratcheted |\n";
        out << "|---|---|---|---|\n";
        for (auto const& gap: gaps)
            out << std::format("| {} | `{}` | {} | {} |\n",
                               nameOf(gap.kind),
                               gap.sequence,
                               gap.count,
                               report.knownGaps.entries.contains(gapKey(gap)) ? "yes" : "**no**");
    }

    return out.str();
}

int exitCode(Report const& report)
{
    if (!staleGaps(report).empty())
        return EXIT_FAILURE;

    // A stale failure-ratchet entry is only meaningful if the suite actually decided that test. If
    // esctest was never fetched, its scenario is skipped and every entry would *look* fixed — failing
    // the build for not having cloned a GPL repository, which is exactly the kind of noise that
    // teaches people to ignore a gate.
    auto const decided =
        std::ranges::any_of(report.scenarios, [](auto const& s) { return s.escTest.valid(); });
    if (decided && !staleFailures(report).empty())
        return EXIT_FAILURE;

    // Gating is decided per scenario, not per suite (@see Scenario::gatesBuild): within one suite the
    // oracles differ in how far they can be trusted. Every scenario is captured at a causal barrier now --
    // the program's own verdicts after an orderly exit, or the marker it flushed on the wire -- so a
    // mismatch means the engine moved, and a scenario may gate on that. A non-gating failure is still
    // rendered -- see renderSummary() -- so a regression is visible in the log the moment it appears.
    for (auto const& scenario: report.scenarios)
        if (scenario.gatesBuild && judge(scenario, report.knownGaps, report.knownFailures) == Outcome::Failed)
            return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

} // namespace vtconformance
