// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/CpuCounters.hpp>

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace core::platform::testing
{

/// A scripted ICpuCounterSource for unit tests.
class FakeCpuCounterSource final: public ICpuCounterSource
{
  public:
    /// @param script What successive read() calls return; the last sample repeats once exhausted.
    /// @param supported The measures that report no unavailable reason.
    FakeCpuCounterSource(std::vector<CpuCounterSample> script, std::vector<CpuCounter> supported):
        _script { std::move(script) }, _supported { std::move(supported) }
    {
    }

    [[nodiscard]] CpuCounterSample read() override
    {
        if (_script.empty())
            return {};
        auto const sample = _script[_next];
        if (_next + 1 < _script.size())
            ++_next;
        return sample;
    }

    [[nodiscard]] std::string_view unavailableReason(CpuCounter counter) const noexcept override
    {
        return std::ranges::find(_supported, counter) != std::ranges::end(_supported) ? std::string_view {}
                                                                                      : "fake: unsupported";
    }

    [[nodiscard]] std::string_view note() const noexcept override { return {}; }

  private:
    std::vector<CpuCounterSample> _script;
    std::vector<CpuCounter> _supported;
    std::size_t _next = 0;
};

} // namespace core::platform::testing
