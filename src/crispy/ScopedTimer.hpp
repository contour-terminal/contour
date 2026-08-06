// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.hpp>

#include <chrono>
#include <string_view>

namespace crispy
{

/// RAII utility that logs the elapsed time of a scope to a given log category.
class ScopedTimer
{
  public:
    /// Constructs a ScopedTimer that logs the elapsed duration on destruction.
    ///
    /// @param category The logstore category to log the timing to.
    /// @param label A human-readable label identifying the timed section.
    ScopedTimer(logstore::Category const& category, std::string_view label):
        _category { category }, _label { label }, _start { std::chrono::steady_clock::now() }
    {
    }

    ~ScopedTimer()
    {
        if (_category.is_enabled())
        {
            auto const elapsed = std::chrono::steady_clock::now() - _start;
            auto const ms =
                static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count())
                / 1000.0;
            _category()("{}: {:.1f} ms", _label, ms);
        }
    }

    ScopedTimer(ScopedTimer const&) = delete;
    ScopedTimer& operator=(ScopedTimer const&) = delete;
    ScopedTimer(ScopedTimer&&) = delete;
    ScopedTimer& operator=(ScopedTimer&&) = delete;

  private:
    logstore::Category const& _category;
    std::string_view _label;
    std::chrono::steady_clock::time_point _start;
};

} // namespace crispy
