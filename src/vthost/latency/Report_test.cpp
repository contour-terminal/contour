// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <string>
#include <vector>

#include <vthost/latency/Report.hpp>

using namespace vthost::latency;
using namespace std::chrono_literals;

TEST_CASE("a probe report round-trips and a malformed one names its error", "[vthost][latency]")
{
    auto clock = core::platform::ManualClock { core::platform::SteadyTimePoint {} + 1s };
    auto timeline = StartupTimeline { clock, clock.now() };
    clock.advance(40ms);
    timeline.mark(StartupMark::WindowCreated);
    auto const result = ProbeResult { .phase = ProbePhase::Finished,
                                      .firstPrompt = clock.now() + 3ms,
                                      .keystrokes = { 7ms, 9ms },
                                      .awaitedColumn = -1 };

    auto const parsed = parseProbeReport(formatProbeReport(result, timeline));
    REQUIRE(parsed.has_value());
    CHECK(parsed->phase == ProbePhase::Finished);
    CHECK(parsed->processStart == core::platform::SteadyTimePoint {} + 1s);
    CHECK(parsed->firstPrompt == result.firstPrompt);
    CHECK(parsed->keystrokes == result.keystrokes);
    CHECK(parsed->awaitedColumn == -1);

    CHECK(parseProbeReport("phase finished\n").error() == ReportError::MissingField);
    CHECK(parseProbeReport("phase sleepy\nprocess-start-ns 1\n").error() == ReportError::UnknownPhase);
    CHECK(parseProbeReport("keystroke-ns x\n").error() == ReportError::Malformed);
    CHECK(parseProbeReport("phase finished\nprocess-start-ns 1\nmark nowhere 5\n").error()
          == ReportError::Malformed);
    CHECK(parseProbeReport("phase finished\nprocess-start-ns 1\nbogus 5\n").error()
          == ReportError::Malformed);
}

TEST_CASE("a timed-out report keeps the column it waited for", "[vthost][latency]")
{
    auto clock = core::platform::ManualClock {};
    auto const timeline = StartupTimeline { clock, clock.now() };
    auto const result = ProbeResult {
        .phase = ProbePhase::EchoTimedOut, .firstPrompt = std::nullopt, .keystrokes = {}, .awaitedColumn = 3
    };
    auto const parsed = parseProbeReport(formatProbeReport(result, timeline));
    REQUIRE(parsed.has_value());
    CHECK(parsed->phase == ProbePhase::EchoTimedOut);
    CHECK(parsed->awaitedColumn == 3);
    CHECK_FALSE(parsed->firstPrompt.has_value());
}

TEST_CASE("formatRecord and formatGateLine print the documented text", "[vthost][latency]")
{
    auto const summary = Summary { .count = 200, .p50 = 410us, .p90 = 800us, .p99 = 1200us, .max = 2500us };
    CHECK(formatRecord("keystroke", "daemon", summary)
          == "keystroke\tdaemon\t200\t410.0\t800.0\t1200.0\t2500.0\n");

    auto const pass = GateResult {
        .startup = Verdict::Pass, .keystroke = Verdict::Pass, .startupDelta = 3200us, .keystrokeDelta = 410us
    };
    CHECK(formatGateLine(Budgets {}, pass)
          == "gate: startup +3.2 ms (budget +10.0 ms) PASS; keystroke p50 +0.41 ms (budget +1.00 ms) PASS");

    auto const mixed = GateResult { .startup = Verdict::Fail,
                                    .keystroke = Verdict::Inconclusive,
                                    .startupDelta = 11ms,
                                    .keystrokeDelta = 1500us };
    CHECK(formatGateLine(Budgets {}, mixed)
          == "gate: startup +11.0 ms (budget +10.0 ms) FAIL; keystroke p50 +1.50 ms (budget +1.00 ms) "
             "INCONCLUSIVE");
}

TEST_CASE("the terminal table lines its columns up under the header", "[vthost][latency]")
{
    CHECK(formatTableHeader()
          == "metric          mode         count     p50 us     p90 us     p99 us     max us\n");
    auto const summary = Summary { .count = 200, .p50 = 410us, .p90 = 800us, .p99 = 1200us, .max = 2500us };
    CHECK(formatTableRow("keystroke", "daemon", summary)
          == "keystroke       daemon         200      410.0      800.0     1200.0     2500.0\n");
    CHECK(formatScalarRecord("flood-mibps", "daemon", 3, 812.34) == "flood-mibps\tdaemon\t3\t812.3\n");
}
