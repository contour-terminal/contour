// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/CpuCounters.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace core::platform::detail
{

/// A counter source that reports nothing and says why; every backend's "nothing works" path.
class UnavailableCpuCounterSource final: public ICpuCounterSource
{
  public:
    /// @param reason Why no measure is available; returned for every measure.
    explicit UnavailableCpuCounterSource(std::string reason): _reason { std::move(reason) } {}

    [[nodiscard]] CpuCounterSample read() override { return {}; }

    [[nodiscard]] std::string_view unavailableReason(CpuCounter /*counter*/) const noexcept override
    {
        return _reason;
    }

    [[nodiscard]] std::string_view note() const noexcept override { return {}; }

  private:
    std::string _reason;
};

} // namespace core::platform::detail
