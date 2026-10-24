// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Quantiles of latency samples.

#include <core/platform/Clock.hpp>

#include <cstddef>
#include <vector>

namespace vthost::latency
{

/// The shape of a set of samples.
struct Summary
{
    std::size_t count = 0;
    core::platform::SteadyDuration p50 {};
    core::platform::SteadyDuration p90 {};
    core::platform::SteadyDuration p99 {};
    core::platform::SteadyDuration max {};
};

/// Summarizes @p samples with nearest-rank quantiles.
/// @param samples The measurements, in any order.
/// @return Their count, quantiles and maximum; all zero for no samples.
[[nodiscard]] Summary summarize(std::vector<core::platform::SteadyDuration> samples);

} // namespace vthost::latency
