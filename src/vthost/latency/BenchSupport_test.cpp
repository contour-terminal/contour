// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <vthost/latency/BenchSupport.hpp>
#include <vthost/latency/EchoProtocol.hpp>

using namespace vthost::latency;
using namespace std::chrono_literals;

namespace
{
ProbeResult settledAs(ProbePhase phase, int awaitedColumn = -1)
{
    auto result = ProbeResult {};
    result.phase = phase;
    result.awaitedColumn = awaitedColumn;
    return result;
}
} // namespace

TEST_CASE("judgeProbe names the column a timed-out probe waited for", "[vthost][latency]")
{
    CHECK(judgeProbe(settledAs(ProbePhase::Finished)).has_value());

    auto const echo = judgeProbe(settledAs(ProbePhase::EchoTimedOut, 7));
    REQUIRE_FALSE(echo.has_value());
    CHECK(echo.error() == "probe timed out waiting for cursor column 7");

    auto const prompt = judgeProbe(settledAs(ProbePhase::PromptTimedOut));
    REQUIRE_FALSE(prompt.has_value());
    CHECK(prompt.error() == "probe timed out waiting for cursor column 2 (the prompt)");

    CHECK_FALSE(judgeProbe(settledAs(ProbePhase::AwaitingEcho)).has_value());
}

TEST_CASE("only a probe that has said all it will is settled", "[vthost][latency]")
{
    CHECK(isSettled(ProbePhase::Finished));
    CHECK(isSettled(ProbePhase::PromptTimedOut));
    CHECK(isSettled(ProbePhase::EchoTimedOut));
    CHECK_FALSE(isSettled(ProbePhase::AwaitingPrompt));
    CHECK_FALSE(isSettled(ProbePhase::Pausing));
    CHECK_FALSE(isSettled(ProbePhase::AwaitingEcho));
}

TEST_CASE("medianOf takes the middle figure", "[vthost][latency]")
{
    CHECK(medianOf({}) == 0.0);
    CHECK(medianOf({ 5.0 }) == 5.0);
    CHECK(medianOf({ 9.0, 1.0, 5.0 }) == 5.0);
}

TEST_CASE("publish prints the table and the gate line, and writes the same to the record",
          "[vthost][latency]")
{
    auto report = BenchReport {};
    report.series = { MetricSeries {
        .metric = "keystroke", .mode = "daemon", .samples = { 100us, 300us, 200us } } };
    report.scalars = { ScalarSeries {
        .metric = "flood-mibps", .mode = "daemon", .values = { 10.0, 30.0, 20.0 } } };
    report.gate = GateResult { .startup = Verdict::Inconclusive, .keystroke = Verdict::Inconclusive };
    report.gateNote = "(informational)";

    auto const record = std::filesystem::temp_directory_path() / "bench-support-test-record.tsv";
    auto out = std::ostringstream {};
    REQUIRE(publish(report, record, out).has_value());

    auto const text = out.str();
    CHECK(text.starts_with("metric          mode         count     p50 us"));
    CHECK(text.contains("keystroke       daemon           3      200.0"));
    CHECK(text.contains("flood-mibps     daemon           3       20.0"));
    CHECK(text.contains("INCONCLUSIVE (informational)\n"));

    auto written = std::ostringstream {};
    written << std::ifstream(record, std::ios::binary).rdbuf();
    CHECK(written.str().starts_with("keystroke\tdaemon\t3\t200.0\t"));
    CHECK(written.str().contains("flood-mibps\tdaemon\t3\t20.0\n"));
    CHECK(written.str().contains("(informational)"));
    std::filesystem::remove(record);
}

TEST_CASE("publish says when the record cannot be written", "[vthost][latency]")
{
    auto out = std::ostringstream {};
    auto const result =
        publish(BenchReport {}, std::filesystem::path { "/nonexistent-directory/record.tsv" }, out);
    CHECK_FALSE(result.has_value());
}
