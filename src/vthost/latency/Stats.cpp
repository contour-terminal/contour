// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/Stats.hpp>

#include <algorithm>
#include <cmath>

namespace vthost::latency
{

namespace
{
    /// The nearest-rank quantile of sorted, non-empty samples: the smallest sample that at least
    /// @p fraction of them do not exceed.
    [[nodiscard]] core::platform::SteadyDuration quantile(
        std::vector<core::platform::SteadyDuration> const& sorted, double fraction)
    {
        auto const rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
        return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
    }
} // namespace

Summary summarize(std::vector<core::platform::SteadyDuration> samples)
{
    if (samples.empty())
        return Summary {};
    std::ranges::sort(samples);
    return Summary { .count = samples.size(),
                     .p50 = quantile(samples, 0.50),
                     .p90 = quantile(samples, 0.90),
                     .p99 = quantile(samples, 0.99),
                     .max = samples.back() };
}

} // namespace vthost::latency
