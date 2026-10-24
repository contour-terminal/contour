// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/Gate.hpp>

#include <vthost/latency/Stats.hpp>

namespace vthost::latency
{

namespace
{
    struct Judged
    {
        Verdict verdict;
        core::platform::SteadyDuration delta;
    };

    [[nodiscard]] Judged judgeMedian(core::platform::SteadyDuration budget,
                                     std::size_t minimumSamples,
                                     std::vector<core::platform::SteadyDuration> const& inProcess,
                                     std::vector<core::platform::SteadyDuration> const& daemon)
    {
        auto const delta = summarize(daemon).p50 - summarize(inProcess).p50;
        if (inProcess.size() < minimumSamples || daemon.size() < minimumSamples)
            return Judged { .verdict = Verdict::Inconclusive, .delta = delta };
        return Judged { .verdict = delta <= budget ? Verdict::Pass : Verdict::Fail, .delta = delta };
    }
} // namespace

GateResult judge(Budgets const& budgets, ModeSamples const& inProcess, ModeSamples const& daemon)
{
    auto const startup =
        judgeMedian(budgets.startup, MinimumStartupSamples, inProcess.startup, daemon.startup);
    auto const keystroke =
        judgeMedian(budgets.keystrokeMedian, MinimumKeystrokeSamples, inProcess.keystroke, daemon.keystroke);
    return GateResult { .startup = startup.verdict,
                        .keystroke = keystroke.verdict,
                        .startupDelta = startup.delta,
                        .keystrokeDelta = keystroke.delta };
}

} // namespace vthost::latency
