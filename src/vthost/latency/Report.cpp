// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/Report.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <ranges>
#include <system_error>

namespace vthost::latency
{

namespace
{
    struct PhaseRow
    {
        ProbePhase phase;
        std::string_view name;
    };

    constexpr auto PhaseTable = std::array {
        PhaseRow { .phase = ProbePhase::AwaitingPrompt, .name = "awaiting-prompt" },
        PhaseRow { .phase = ProbePhase::Pausing, .name = "pausing" },
        PhaseRow { .phase = ProbePhase::AwaitingEcho, .name = "awaiting-echo" },
        PhaseRow { .phase = ProbePhase::Finished, .name = "finished" },
        PhaseRow { .phase = ProbePhase::PromptTimedOut, .name = "prompt-timed-out" },
        PhaseRow { .phase = ProbePhase::EchoTimedOut, .name = "echo-timed-out" },
    };

    struct VerdictRow
    {
        Verdict verdict;
        std::string_view name;
    };

    constexpr auto VerdictTable = std::array {
        VerdictRow { .verdict = Verdict::Pass, .name = "PASS" },
        VerdictRow { .verdict = Verdict::Fail, .name = "FAIL" },
        VerdictRow { .verdict = Verdict::Inconclusive, .name = "INCONCLUSIVE" },
    };

    [[nodiscard]] std::string_view nameOf(ProbePhase phase)
    {
        return std::ranges::find(PhaseTable, phase, &PhaseRow::phase)->name;
    }

    [[nodiscard]] std::string_view nameOf(Verdict verdict)
    {
        return std::ranges::find(VerdictTable, verdict, &VerdictRow::verdict)->name;
    }

    [[nodiscard]] double microseconds(core::platform::SteadyDuration duration)
    {
        return std::chrono::duration<double, std::micro> { duration }.count();
    }

    [[nodiscard]] double milliseconds(core::platform::SteadyDuration duration)
    {
        return std::chrono::duration<double, std::milli> { duration }.count();
    }

    template <typename Number>
    [[nodiscard]] std::optional<Number> parseNumber(std::string_view text)
    {
        auto value = Number {};
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc {} || end != text.data() + text.size())
            return std::nullopt;
        return value;
    }

    [[nodiscard]] core::platform::SteadyTimePoint timePointOfNanoseconds(int64_t nanoseconds)
    {
        return core::platform::SteadyTimePoint { std::chrono::duration_cast<core::platform::SteadyDuration>(
            std::chrono::nanoseconds { nanoseconds }) };
    }

    [[nodiscard]] int64_t nanosecondsOf(core::platform::SteadyDuration duration)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    }

    [[nodiscard]] int64_t nanosecondsOf(core::platform::SteadyTimePoint point)
    {
        return nanosecondsOf(point.time_since_epoch());
    }
} // namespace

std::string formatProbeReport(ProbeResult const& result, StartupTimeline const& timeline)
{
    auto const processStart = *timeline.at(StartupMark::ProcessStart);
    auto text =
        std::format("phase {}\nprocess-start-ns {}\n", nameOf(result.phase), nanosecondsOf(processStart));
    if (result.firstPrompt)
        text += std::format("first-prompt-ns {}\n", nanosecondsOf(*result.firstPrompt));
    text += std::format("awaited-column {}\n", result.awaitedColumn);
    for (auto const keystroke: result.keystrokes)
        text += std::format("keystroke-ns {}\n", nanosecondsOf(keystroke));
    for (auto const& row: StartupMarkTable)
        if (auto const stamp = timeline.at(row.mark); stamp)
            text += std::format("mark {} {}\n", row.name, nanosecondsOf(*stamp - processStart));
    return text;
}

std::expected<ProbeReport, ReportError> parseProbeReport(std::string_view text)
{
    auto report = ProbeReport {};
    auto sawPhase = false;
    auto sawProcessStart = false;
    for (auto const line: std::views::split(text, '\n') | std::views::transform([](auto const& part) {
                              return std::string_view { part.begin(), part.end() };
                          }))
    {
        if (line.empty())
            continue;
        auto const space = line.find(' ');
        if (space == std::string_view::npos)
            return std::unexpected(ReportError::Malformed);
        auto const key = line.substr(0, space);
        auto const value = line.substr(space + 1);
        if (key == "phase")
        {
            auto const row = std::ranges::find(PhaseTable, value, &PhaseRow::name);
            if (row == PhaseTable.end())
                return std::unexpected(ReportError::UnknownPhase);
            report.phase = row->phase;
            sawPhase = true;
        }
        else if (key == "process-start-ns")
        {
            auto const nanoseconds = parseNumber<int64_t>(value);
            if (!nanoseconds)
                return std::unexpected(ReportError::Malformed);
            report.processStart = timePointOfNanoseconds(*nanoseconds);
            sawProcessStart = true;
        }
        else if (key == "first-prompt-ns")
        {
            auto const nanoseconds = parseNumber<int64_t>(value);
            if (!nanoseconds)
                return std::unexpected(ReportError::Malformed);
            report.firstPrompt = timePointOfNanoseconds(*nanoseconds);
        }
        else if (key == "awaited-column")
        {
            auto const column = parseNumber<int>(value);
            if (!column)
                return std::unexpected(ReportError::Malformed);
            report.awaitedColumn = *column;
        }
        else if (key == "keystroke-ns")
        {
            auto const nanoseconds = parseNumber<int64_t>(value);
            if (!nanoseconds)
                return std::unexpected(ReportError::Malformed);
            report.keystrokes.push_back(std::chrono::duration_cast<core::platform::SteadyDuration>(
                std::chrono::nanoseconds { *nanoseconds }));
        }
        else if (key == "mark")
        {
            auto const gap = value.find(' ');
            if (gap == std::string_view::npos
                || std::ranges::find(StartupMarkTable, value.substr(0, gap), &StartupMarkRow::name)
                       == StartupMarkTable.end()
                || !parseNumber<int64_t>(value.substr(gap + 1)))
                return std::unexpected(ReportError::Malformed);
        }
        else
            return std::unexpected(ReportError::Malformed);
    }
    if (!sawPhase || !sawProcessStart)
        return std::unexpected(ReportError::MissingField);
    return report;
}

std::string formatRecord(std::string_view metric, std::string_view mode, Summary const& summary)
{
    return std::format("{}\t{}\t{}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.1f}\n",
                       metric,
                       mode,
                       summary.count,
                       microseconds(summary.p50),
                       microseconds(summary.p90),
                       microseconds(summary.p99),
                       microseconds(summary.max));
}

std::string formatScalarRecord(std::string_view metric,
                               std::string_view mode,
                               std::size_t count,
                               double value)
{
    return std::format("{}\t{}\t{}\t{:.1f}\n", metric, mode, count, value);
}

std::string formatTableHeader()
{
    return std::format("{:<15} {:<12} {:>5} {:>10} {:>10} {:>10} {:>10}\n",
                       "metric",
                       "mode",
                       "count",
                       "p50 us",
                       "p90 us",
                       "p99 us",
                       "max us");
}

std::string formatTableRow(std::string_view metric, std::string_view mode, Summary const& summary)
{
    return std::format("{:<15} {:<12} {:>5} {:>10.1f} {:>10.1f} {:>10.1f} {:>10.1f}\n",
                       metric,
                       mode,
                       summary.count,
                       microseconds(summary.p50),
                       microseconds(summary.p90),
                       microseconds(summary.p99),
                       microseconds(summary.max));
}

std::string formatGateLine(Budgets const& budgets, GateResult const& result)
{
    return std::format("gate: startup {:+.1f} ms (budget {:+.1f} ms) {}; keystroke p50 {:+.2f} ms (budget "
                       "{:+.2f} ms) {}",
                       milliseconds(result.startupDelta),
                       milliseconds(budgets.startup),
                       nameOf(result.startup),
                       milliseconds(result.keystrokeDelta),
                       milliseconds(budgets.keystrokeMedian),
                       nameOf(result.keystroke));
}

} // namespace vthost::latency
